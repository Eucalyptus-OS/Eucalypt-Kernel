#ifndef MEMMAP_H
#define MEMMAP_H

#include <stdint.h>

void *phys_to_virt(uint64_t phys_addr);

#endif