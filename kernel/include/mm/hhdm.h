#pragma once

#include <stdint.h>

void hhdm_init();
uint64_t phys_virt(uint64_t phys);

uint64_t virt_phys(uint64_t virt);