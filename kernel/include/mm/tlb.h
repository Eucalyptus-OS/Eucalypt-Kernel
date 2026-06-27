#pragma once

#include <stddef.h>
#include <stdint.h>

void invalidate(uint64_t addr, size_t length);
void tlb_shootdown(uint64_t addr, size_t length);