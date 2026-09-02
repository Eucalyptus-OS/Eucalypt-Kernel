#pragma once

#include <stdint.h>

extern uint64_t hhdm_offset;

uint8_t hhdm_init(uint64_t offset);
void *phys_to_virt(uintptr_t phys);
uintptr_t virt_to_phys(void *virt);