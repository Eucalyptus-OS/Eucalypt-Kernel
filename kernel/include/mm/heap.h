#pragma once

#include <stdint.h>

// Allocate size bytes (16-byte aligned); free the result with kfree
void *kmalloc(uintptr_t size);
// Release a pointer previously returned by kmalloc
void kfree(void *addr);