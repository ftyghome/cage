/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200809L

#include "keylog.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/util/log.h>

/* Lock-free SPSC queue implementation */
static bool
queue_push(struct cg_keylog_queue *queue, struct cg_key_event *event)
{
	uint32_t write_pos = atomic_load_explicit(&queue->write_pos, memory_order_relaxed);
	uint32_t read_pos = atomic_load_explicit(&queue->read_pos, memory_order_acquire);
	uint32_t next_write = (write_pos + 1) % KEYLOG_QUEUE_SIZE;

	if (next_write == read_pos) {
		return false; /* Queue full */
	}

	queue->buffer[write_pos] = *event;
	atomic_store_explicit(&queue->write_pos, next_write, memory_order_release);
	return true;
}

static bool
queue_pop(struct cg_keylog_queue *queue, struct cg_key_event *event)
{
	uint32_t read_pos = atomic_load_explicit(&queue->read_pos, memory_order_relaxed);
	uint32_t write_pos = atomic_load_explicit(&queue->write_pos, memory_order_acquire);

	if (read_pos == write_pos) {
		return false; /* Queue empty */
	}

	*event = queue->buffer[read_pos];
	uint32_t next_read = (read_pos + 1) % KEYLOG_QUEUE_SIZE;
	atomic_store_explicit(&queue->read_pos, next_read, memory_order_release);
	return true;
}

/* Buffer management */
static void
flush_write_buffer(struct cg_keylog *keylog)
{
	pthread_mutex_lock(&keylog->write_buffer.mutex);
	
	if (keylog->write_buffer.count > 0 && keylog->record_file) {
		size_t written = fwrite(keylog->write_buffer.records,
		                        sizeof(struct cg_key_record),
		                        keylog->write_buffer.count,
		                        keylog->record_file);
		if (written != keylog->write_buffer.count) {
			wlr_log(WLR_ERROR, "Failed to write all records to file");
		}
		fflush(keylog->record_file);
		keylog->write_buffer.count = 0;
	}
	
	pthread_mutex_unlock(&keylog->write_buffer.mutex);
}

static void
buffer_key_record(struct cg_keylog *keylog, struct cg_key_record *record)
{
	pthread_mutex_lock(&keylog->write_buffer.mutex);
	
	keylog->write_buffer.records[keylog->write_buffer.count++] = *record;
	
	if (keylog->write_buffer.count >= KEYLOG_BUFFER_SIZE) {
		/* Buffer full, flush to disk */
		if (keylog->record_file) {
			size_t written = fwrite(keylog->write_buffer.records,
			                        sizeof(struct cg_key_record),
			                        keylog->write_buffer.count,
			                        keylog->record_file);
			if (written != keylog->write_buffer.count) {
				wlr_log(WLR_ERROR, "Failed to write all records to file");
			}
			fflush(keylog->record_file);
		}
		keylog->write_buffer.count = 0;
	}
	
	pthread_mutex_unlock(&keylog->write_buffer.mutex);
}

static void
start_recording(struct cg_keylog *keylog, const char *record_path)
{
	pthread_mutex_lock(&keylog->record_mutex);
	
	if (atomic_load(&keylog->recording)) {
		wlr_log(WLR_INFO, "Already recording");
		pthread_mutex_unlock(&keylog->record_mutex);
		return;
	}
	
	/* Build full path for keylog.bin */
	snprintf(keylog->record_path, sizeof(keylog->record_path), "%s/keylog.bin", record_path);
	
	/* Open file in append mode */
	keylog->record_file = fopen(keylog->record_path, "ab");
	if (!keylog->record_file) {
		wlr_log(WLR_ERROR, "Failed to open keylog file: %s", keylog->record_path);
		pthread_mutex_unlock(&keylog->record_mutex);
		return;
	}
	
	atomic_store(&keylog->recording, true);
	wlr_log(WLR_INFO, "Started recording to: %s", keylog->record_path);
	
	pthread_mutex_unlock(&keylog->record_mutex);
}

static void
stop_recording(struct cg_keylog *keylog)
{
	pthread_mutex_lock(&keylog->record_mutex);
	
	if (!atomic_load(&keylog->recording)) {
		pthread_mutex_unlock(&keylog->record_mutex);
		return;
	}
	
	/* Flush remaining buffer */
	flush_write_buffer(keylog);
	
	/* Close file */
	if (keylog->record_file) {
		fclose(keylog->record_file);
		keylog->record_file = NULL;
	}
	
	atomic_store(&keylog->recording, false);
	wlr_log(WLR_INFO, "Stopped recording to: %s", keylog->record_path);
	
	pthread_mutex_unlock(&keylog->record_mutex);
}

/* DBus signal handler */
static void
on_dbus_signal(GDBusConnection *connection,
               const gchar *sender_name,
               const gchar *object_path,
               const gchar *interface_name,
               const gchar *signal_name,
               GVariant *parameters,
               gpointer user_data)
{
	struct cg_keylog *keylog = user_data;
	
	wlr_log(WLR_INFO, "DBus signal received: sender=%s, path=%s, interface=%s, signal=%s",
	        sender_name ? sender_name : "(null)",
	        object_path ? object_path : "(null)",
	        interface_name ? interface_name : "(null)",
	        signal_name ? signal_name : "(null)");
	
	if (g_strcmp0(signal_name, "RecordingStarted") == 0) {
		const gchar *record_name = NULL;
		const gchar *record_path = NULL;
		g_variant_get(parameters, "(&s&s)", &record_name, &record_path);
		
		wlr_log(WLR_INFO, "RecordingStarted signal received: name=%s, path=%s",
		        record_name, record_path);
		start_recording(keylog, record_path);
	} else if (g_strcmp0(signal_name, "RecordingStopped") == 0) {
		wlr_log(WLR_INFO, "RecordingStopped signal received");
		stop_recording(keylog);
	}
}

/* DBus monitoring thread */
static void *
dbus_thread_func(void *arg)
{
	struct cg_keylog *keylog = arg;
	
	wlr_log(WLR_INFO, "DBus monitoring thread started");
	
	/* Get connection unique name for debugging */
	const gchar *unique_name = g_dbus_connection_get_unique_name(keylog->dbus_connection);
	wlr_log(WLR_INFO, "DBus connection unique name: %s", unique_name ? unique_name : "(null)");
	
	/* Create GMainLoop for this thread */
	keylog->dbus_loop = g_main_loop_new(NULL, FALSE);
	if (!keylog->dbus_loop) {
		wlr_log(WLR_ERROR, "Failed to create GMainLoop for DBus thread");
		return NULL;
	}
	
	/* Subscribe to signals */
	keylog->signal_subscription_id = g_dbus_connection_signal_subscribe(
		keylog->dbus_connection,
		NULL, /* sender */
		"com.pavus.recorder", /* interface */
		NULL, /* member (signal name) - NULL means all signals */
		"/com/pavus/recorder", /* object_path */
		NULL, /* arg0 */
		G_DBUS_SIGNAL_FLAGS_NONE,
		on_dbus_signal,
		keylog,
		NULL);
	
	if (keylog->signal_subscription_id == 0) {
		wlr_log(WLR_ERROR, "Failed to subscribe to DBus signals");
		g_main_loop_unref(keylog->dbus_loop);
		keylog->dbus_loop = NULL;
		return NULL;
	}
	
	wlr_log(WLR_INFO, "Subscribed to com.pavus.recorder signals (subscription_id=%u)",
	        keylog->signal_subscription_id);
	
	/* Run GMainLoop to process DBus signals */
	wlr_log(WLR_INFO, "Starting GMainLoop for DBus signal processing");
	g_main_loop_run(keylog->dbus_loop);
	
	/* Cleanup after loop exits */
	wlr_log(WLR_INFO, "GMainLoop exited");
	
	/* Unsubscribe */
	if (keylog->signal_subscription_id != 0) {
		g_dbus_connection_signal_unsubscribe(keylog->dbus_connection,
		                                     keylog->signal_subscription_id);
		keylog->signal_subscription_id = 0;
	}
	
	if (keylog->dbus_loop) {
		g_main_loop_unref(keylog->dbus_loop);
		keylog->dbus_loop = NULL;
	}
	
	wlr_log(WLR_INFO, "DBus monitoring thread stopped");
	return NULL;
}

/* Consumer thread - processes key events from queue */
static void *
consumer_thread_func(void *arg)
{
	struct cg_keylog *keylog = arg;
	struct cg_key_event event;
	struct timespec ts;

	wlr_log(WLR_INFO, "Keylog consumer thread started");

	while (atomic_load_explicit(&keylog->running, memory_order_acquire)) {
		if (queue_pop(&keylog->queue, &event)) {
			/* Get timestamp */
			clock_gettime(CLOCK_REALTIME, &ts);

			/* Check if recording */
			if (atomic_load(&keylog->recording)) {
				/* Write to file in binary format */
				struct cg_key_record record = {
					.timestamp_sec = ts.tv_sec,
					.timestamp_nsec = ts.tv_nsec,
					.keycode = event.keycode,
					.state = event.state,
					.padding = 0
				};
				buffer_key_record(keylog, &record);
			}
		} else {
			/* Sleep briefly to avoid busy-waiting */
			struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = 1000000}; /* 1ms */
			nanosleep(&sleep_time, NULL);
		}
	}

	wlr_log(WLR_INFO, "Keylog consumer thread stopped");
	return NULL;
}

bool
keylog_init(struct cg_keylog *keylog, GDBusConnection *dbus_connection)
{
	memset(keylog, 0, sizeof(*keylog));
	
	/* Initialize queue */
	atomic_init(&keylog->queue.write_pos, 0);
	atomic_init(&keylog->queue.read_pos, 0);
	atomic_init(&keylog->running, true);
	atomic_init(&keylog->recording, false);
	
	/* Initialize mutexes */
	pthread_mutex_init(&keylog->record_mutex, NULL);
	pthread_mutex_init(&keylog->write_buffer.mutex, NULL);
	
	/* Store DBus connection */
	keylog->dbus_connection = dbus_connection;
	
	/* Create XKB context and keymap for thread */
	keylog->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!keylog->xkb_context) {
		wlr_log(WLR_ERROR, "Failed to create XKB context for keylog");
		return false;
	}

	keylog->xkb_keymap = xkb_keymap_new_from_names(keylog->xkb_context, NULL,
						       XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keylog->xkb_keymap) {
		wlr_log(WLR_ERROR, "Failed to create XKB keymap for keylog");
		xkb_context_unref(keylog->xkb_context);
		return false;
	}

	/* Start consumer thread */
	if (pthread_create(&keylog->consumer_thread, NULL, consumer_thread_func, keylog) != 0) {
		wlr_log(WLR_ERROR, "Failed to create keylog consumer thread");
		xkb_keymap_unref(keylog->xkb_keymap);
		xkb_context_unref(keylog->xkb_context);
		return false;
	}
	
	/* Start DBus monitoring thread if connection available */
	if (keylog->dbus_connection) {
		if (pthread_create(&keylog->dbus_thread, NULL, dbus_thread_func, keylog) != 0) {
			wlr_log(WLR_ERROR, "Failed to create DBus monitoring thread");
			atomic_store(&keylog->running, false);
			pthread_join(keylog->consumer_thread, NULL);
			xkb_keymap_unref(keylog->xkb_keymap);
			xkb_context_unref(keylog->xkb_context);
			return false;
		}
	} else {
		wlr_log(WLR_INFO, "No DBus connection available, keylog recording disabled");
	}

	wlr_log(WLR_INFO, "Keylog initialized");
	return true;
}

void
keylog_destroy(struct cg_keylog *keylog)
{
	if (!keylog) {
		return;
	}

	/* Stop threads */
	atomic_store_explicit(&keylog->running, false, memory_order_release);
	
	/* Quit GMainLoop if it's running */
	if (keylog->dbus_loop && g_main_loop_is_running(keylog->dbus_loop)) {
		g_main_loop_quit(keylog->dbus_loop);
	}
	
	pthread_join(keylog->consumer_thread, NULL);
	
	if (keylog->dbus_connection) {
		pthread_join(keylog->dbus_thread, NULL);
	}
	
	/* Stop any ongoing recording */
	if (atomic_load(&keylog->recording)) {
		stop_recording(keylog);
	}
	
	/* Clean up mutexes */
	pthread_mutex_destroy(&keylog->record_mutex);
	pthread_mutex_destroy(&keylog->write_buffer.mutex);

	/* Clean up XKB */
	if (keylog->xkb_keymap) {
		xkb_keymap_unref(keylog->xkb_keymap);
	}
	if (keylog->xkb_context) {
		xkb_context_unref(keylog->xkb_context);
	}

	wlr_log(WLR_INFO, "Keylog destroyed");
}

bool
keylog_push(struct cg_keylog *keylog, uint32_t keycode, uint32_t state)
{
	struct cg_key_event event = {
		.keycode = keycode,
		.state = state,
	};

	if (!queue_push(&keylog->queue, &event)) {
		wlr_log(WLR_ERROR, "Keylog queue full, dropping event");
		return false;
	}

	return true;
}
