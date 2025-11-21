#ifndef CG_KEYLOG_H
#define CG_KEYLOG_H

#include <gio/gio.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define KEYLOG_QUEUE_SIZE 1024
#define KEYLOG_BUFFER_SIZE 200

/* Compact key event structure for queue */
struct cg_key_event {
	uint32_t keycode;
	uint32_t state; /* WL_KEYBOARD_KEY_STATE_PRESSED or RELEASED */
};

/* Binary format for file storage (16 bytes per event) */
struct cg_key_record {
	uint64_t timestamp_sec;  /* 8 bytes */
	uint32_t timestamp_nsec; /* 4 bytes */
	uint16_t keycode;        /* 2 bytes */
	uint8_t state;           /* 1 byte */
	uint8_t padding;         /* 1 byte for alignment */
} __attribute__((packed));

/* Lock-free SPSC queue */
struct cg_keylog_queue {
	struct cg_key_event buffer[KEYLOG_QUEUE_SIZE];
	_Atomic uint32_t write_pos;
	_Atomic uint32_t read_pos;
	char padding[64]; /* Cache line padding */
};

/* File write buffer */
struct cg_keylog_write_buffer {
	struct cg_key_record records[KEYLOG_BUFFER_SIZE];
	size_t count;
	pthread_mutex_t mutex;
};

/* Keylog thread context */
struct cg_keylog {
	struct cg_keylog_queue queue;
	pthread_t consumer_thread;
	pthread_t dbus_thread;
	_Atomic bool running;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;

	/* Recording state */
	_Atomic bool recording;
	char record_path[512];
	FILE *record_file;
	pthread_mutex_t record_mutex;
	struct cg_keylog_write_buffer write_buffer;

	/* DBus connection */
	GDBusConnection *dbus_connection;
	guint signal_subscription_id;
	GMainLoop *dbus_loop;
};

bool keylog_init(struct cg_keylog *keylog, GDBusConnection *dbus_connection);
void keylog_destroy(struct cg_keylog *keylog);
bool keylog_push(struct cg_keylog *keylog, uint32_t keycode, uint32_t state);

#endif

