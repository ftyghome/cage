# Keylog Binary Format Specification

## Overview

The keylog system records keyboard events in a compact binary format to minimize storage overhead while maintaining precise timing information.

## File Format

Each key event is stored as a fixed-size binary record of **16 bytes**.

### Binary Record Structure

```
Offset | Size | Type     | Description
-------|------|----------|-----------------------------
0      | 8    | uint64_t | Timestamp (seconds since Unix epoch)
8      | 4    | uint32_t | Timestamp (nanoseconds)
12     | 2    | uint16_t | Keycode (Linux input keycode)
14     | 1    | uint8_t  | State (0=released, 1=pressed)
15     | 1    | uint8_t  | Padding (reserved, set to 0)
```

### Data Types

- **timestamp_sec**: 64-bit unsigned integer representing seconds since Unix epoch (1970-01-01 00:00:00 UTC)
- **timestamp_nsec**: 32-bit unsigned integer representing nanosecond portion of timestamp (0-999999999)
- **keycode**: 16-bit unsigned integer representing Linux input event keycode (not xkb keycode)
- **state**: 8-bit unsigned integer
  - `0` = Key released (WL_KEYBOARD_KEY_STATE_RELEASED)
  - `1` = Key pressed (WL_KEYBOARD_KEY_STATE_PRESSED)
- **padding**: 8-bit padding for alignment (always 0)

### Byte Order

All multi-byte values are stored in **native byte order** (typically little-endian on x86/x64).

## Recording Behavior

### Triggering

Recording is controlled via D-Bus signals:
- **Start**: `com.pavus.recorder.RecordingStarted` signal
- **Stop**: `com.pavus.recorder.RecordingStopped` signal

### Buffering

- Events are buffered in memory (200 events maximum)
- Buffer is flushed to disk when:
  - Buffer reaches 200 events
  - Recording is stopped
- File is opened in append mode to preserve existing data

### File Location

Keylog files are written to: `<record_path>/keylog.bin`

Where `<record_path>` is provided by the `RecordingStarted` D-Bus signal.

## Reading the Binary Format

### C Example

```c
#include <stdio.h>
#include <stdint.h>
#include <time.h>

struct key_record {
    uint64_t timestamp_sec;
    uint32_t timestamp_nsec;
    uint16_t keycode;
    uint8_t state;
    uint8_t padding;
} __attribute__((packed));

void read_keylog(const char *filename) {
    FILE *f = fopen(filename, "rb");
    struct key_record record;
    
    while (fread(&record, sizeof(record), 1, f) == 1) {
        printf("[%ld.%09u] Key %s: keycode=%u\n",
               record.timestamp_sec,
               record.timestamp_nsec,
               record.state ? "pressed" : "released",
               record.keycode);
    }
    
    fclose(f);
}
```

### Python Example

```python
import struct
from datetime import datetime

RECORD_FORMAT = '<QIHBx'  # little-endian: Q=uint64, I=uint32, H=uint16, B=uint8, x=padding
RECORD_SIZE = 16

def read_keylog(filename):
    with open(filename, 'rb') as f:
        while True:
            data = f.read(RECORD_SIZE)
            if len(data) < RECORD_SIZE:
                break
            
            timestamp_sec, timestamp_nsec, keycode, state = struct.unpack(RECORD_FORMAT, data)
            dt = datetime.fromtimestamp(timestamp_sec)
            state_str = "pressed" if state else "released"
            
            print(f"[{dt}.{timestamp_nsec:09d}] Key {state_str}: keycode={keycode}")

read_keylog('keylog.bin')
```

## Storage Efficiency

- **Size per event**: 16 bytes
- **Events per MB**: ~65,536 events
- **Example**: 1 hour of typing at 60 WPM (~300 keystrokes/min) = ~18,000 events = ~281 KB

## Keycode Reference

Keycodes follow the Linux input event code standard (defined in `linux/input-event-codes.h`).

Common examples:
- `30` = KEY_A
- `48` = KEY_B
- `1` = KEY_ESC
- `28` = KEY_ENTER
- `57` = KEY_SPACE

For full keycode mapping, refer to: `/usr/include/linux/input-event-codes.h`

## Implementation Notes

- Recording is thread-safe with mutex protection
- Lock-free queue used between input capture and recording threads
- Zero-copy design minimizes memory allocations
- Events are only written when recording is active (not to /dev/null)

