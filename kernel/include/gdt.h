#pragma once
// GDT/TSS construction, segment reload, and SYSCALL MSR setup.

#include <stdint.h>

// Build the GDT, install a TSS, and reload the segment registers.
void gdt_init();
// Set the kernel stack used the next time this process enters ring 0.
void tss_set_kernel_stack(uintptr_t rsp0);
// Enable SYSCALL/SYSRET and point LSTAR at the asm syscall entry stub.
void syscall_setup();