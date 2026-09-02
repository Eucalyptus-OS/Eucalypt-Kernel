#pragma once

#include <stdint.h>
#include <limine.h>

// Page table entry flags (shared by all MM users).
#define PAGE_PRESENT 0x1
#define PAGE_WRITABLE (0x1 << 1)
#define PAGE_USER (0x1 << 2)
#define PAGE_WRITE_THROUGH (0x1 << 3)
#define PAGE_DISABLE_CACHE (0x1 << 4)
#define PAGE_ACCESSED (0x1 << 5)
#define PAGE_DIRTY (0x1 << 6)
#define PAGE_NXE (1ULL << 63)

#define PAGE_SIZE 0x1000

// Physical address of the kernel PML4 — wrap with phys_to_virt() before
// passing to map_page/free_page.
extern uint64_t *kernel_pml4;

uintptr_t create_pml4();
uint8_t map_page(uint64_t *pml4, void *virt, uintptr_t phys, uint64_t flags);
void free_page(uint64_t *pml4, void *virt);
uint8_t paging_init(struct limine_memmap_response *memmap, struct limine_executable_address_response *exec);
