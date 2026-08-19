#pragma once

#include <stdint.h>
#include <limine.h>

uint8_t paging_init(struct limine_memmap_response *memmap, struct limine_executable_address_response *exec);
