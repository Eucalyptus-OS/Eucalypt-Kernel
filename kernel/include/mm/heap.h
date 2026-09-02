#pragma once

#include <stdint.h>

// Kernel heap lives in its own reserved virtual range, grown page by page.
#define HEAP_VIRT_BASE  0xffff900000000000ULL
#define HEAP_MAX_PAGES  16384   // 64 MiB ceiling

uint8_t heap_init();
void *kmalloc(uintptr_t size);
void kfree(void *addr);
