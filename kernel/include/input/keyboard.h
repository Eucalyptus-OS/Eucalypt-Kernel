#pragma once

#include <stdint.h>

// Modifier key bits reported in the event's mods field.
#define KB_MOD_LCTRL   0x01
#define KB_MOD_LSHIFT  0x02
#define KB_MOD_LALT    0x04
#define KB_MOD_LGUI    0x08
#define KB_MOD_RCTRL   0x10
#define KB_MOD_RSHIFT  0x20
#define KB_MOD_RALT    0x40
#define KB_MOD_RGUI    0x80

// Toggle-lock state bits reported in the event's locks field.
#define KB_LOCK_CAPS    0x01
#define KB_LOCK_NUM     0x02
#define KB_LOCK_SCROLL  0x04

// Set in the key code for E0-prefixed (extended) keys.
#define KB_KEY_EXTENDED 0x80

// Callback invoked per decoded scancode event (key, make/break, mods, locks).
typedef void (*keyboard_event_cb_t)(uint8_t key, uint8_t make, uint8_t mods, uint8_t locks);
extern uint8_t scancode;

void keyboard_init();                             // Reset and configure the PS/2 keyboard.
void keyboard_set_event_cb(keyboard_event_cb_t cb); // Install the event callback.
void keyboard_handler();                          // IRQ handler for the keyboard.
