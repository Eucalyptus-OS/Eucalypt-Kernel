#pragma once

#include <stdint.h>

uint8_t gdt_init();
void tss_set_kernel_stack(uint64_t rsp0);
