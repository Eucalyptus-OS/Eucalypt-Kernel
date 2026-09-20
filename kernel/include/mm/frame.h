#pragma once

#include <limine.h>

// Allocate one physical frame; returns its physical address or 0 on OOM
uintptr_t frame_alloc();
// Allocate count physically contiguous frames; returns the base physical address
uintptr_t frame_alloc_contig(uint64_t count);
// Release a frame previously obtained from frame_alloc/frame_alloc_contig
void frame_free(uintptr_t ptr);
// Initialise the frame allocator from the Limine memory map
void frame_init(struct limine_memmap_response *memmap);