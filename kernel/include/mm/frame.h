#pragma once

#include <stdint.h>
#include <stddef.h>
#include <limine.h>

extern uint64_t frame_list;

uintptr_t frame_alloc();
void frame_free(uintptr_t frame);
uint8_t frame_init(struct limine_memmap_response *memmap);