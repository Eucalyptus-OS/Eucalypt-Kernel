#pragma once

#include <stdint.h>

void gdt_init();
void tss_set_kernel_stack(uintptr_t rsp0);
void syscall_setup();