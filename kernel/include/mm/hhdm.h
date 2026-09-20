#pragma once

#include <stdint.h>

extern uint64_t hhdm_offset; // higher-half offset used by phys_to_virt/virt_to_phys

// Record the kernel's HHDM offset
void hhdm_init(uint64_t offset);
// Translate a physical address to its higher-half virtual alias
void *phys_to_virt(uintptr_t phys);
// Translate a higher-half virtual address back to physical
uintptr_t virt_to_phys(void *virt);