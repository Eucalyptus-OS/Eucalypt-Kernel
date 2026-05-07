#pragma once

#include <stdint.h>

uint64_t frame_alloc();
void frame_free(uint64_t page);
void frame_init();
