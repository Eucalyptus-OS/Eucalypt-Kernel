#pragma once

#include <stdint.h>

void *kmalloc(uintptr_t size);
void kfree(void *addr);