#pragma once

#include <limine.h>

uintptr_t frame_alloc();
uintptr_t frame_alloc_contig(uint64_t count);
void frame_free(uintptr_t ptr);
void frame_init(struct limine_memmap_response *memmap);