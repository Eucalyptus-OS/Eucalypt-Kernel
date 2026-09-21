#pragma once

#include <stdint.h>

// Input event types (subset of Linux input-event-codes.h).
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04

// Event codes used in the non-EV_KEY slots above.
#define SYN_REPORT 0x00
#define MSC_SCAN   0x04

// Relative axis codes (Linux rel-axes.h).
#define REL_X       0
#define REL_Y       1
#define REL_HWHEEL  6
#define REL_WHEEL   8

// Button codes (Linux input-event-codes.h).
#define BTN_LEFT   272
#define BTN_RIGHT  273
#define BTN_MIDDLE 274

// Registered evdev devices: event0 is the keyboard, event1 the PS/2 mouse.
#define EVDEV_KBD 0
#define EVDEV_MSE 1

// One evdev record, byte-compatible with the Linux userspace ABI (24 bytes).
struct input_event {
    int64_t  tv_sec;
    int64_t  tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

// Emit one raw event to an evdev device, waking any blocked reader.
void evdev_emit(uint32_t dev, uint16_t type, uint16_t code, int32_t value);

// Feed one decoded keyboard event into /dev/event0.
// key uses the keyboard.c convention (set-1 scancode, bit 7 = E0-extended);
// value is 1 (press), 0 (release), or 2 (auto-repeat).
void evdev_report(uint8_t key, uint8_t value);

// Register /dev/event0 and /dev/event1; must run after devfs_init.
void evdev_init();