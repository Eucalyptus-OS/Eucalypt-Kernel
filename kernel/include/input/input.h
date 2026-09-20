#pragma once

#include <stdint.h>

// Initialize keyboard input and route events to the TTYs.
void input_init();
// Handle one keyboard event; see keyboard.h for key/modifier codes.
void input_handle_event(uint8_t key, uint8_t make, uint8_t mods, uint8_t locks);
// Pop the next queued character (0 on success, -1 when empty).
int input_getchar(char *c);
// Number of characters waiting in the input queue.
int input_available();
// Empty the input queue.
void input_flush();
