#pragma once

#include <stdint.h>

void input_init();
void input_handle_event(uint8_t key, uint8_t make, uint8_t mods, uint8_t locks);
int input_getchar(char *c);
int input_available();
void input_flush();
