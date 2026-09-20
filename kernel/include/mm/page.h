#pragma once

#include <stdint.h>
#include <limine.h>

#define PAGE_SIZE 0x1000

#define PAGE_PRESENT 0x1
#define PAGE_WRITABLE (0x1 << 1)
#define PAGE_USER (0x1 << 2)
#define PAGE_WRITE_THROUGH (0x1 << 3)
#define PAGE_DISABLE_CACHE (0x1 << 4)
#define PAGE_ACCESSED (0x1 << 5)
#define PAGE_DIRTY (0x1 << 6)
#define PAGE_NXE (1ULL << 63) // execute-disable (No-eXecute) bit in the upper entry half

#define KERNEL_STACK_REGION 0xFFFFFE0000000000UL // fixed higher-half slot reserved for each process's kernel stack

extern uint64_t *kernel_pml4; // physical address of the kernel's root PML4, shared by all address spaces

// Switch address spaces by reloading CR3
void reload_cr3(uint64_t pml_to_load);
// Allocate a fresh PML4 that shares the kernel's higher half
uintptr_t paging_create_pml4();
// Deep-copy the current user address space into a new PML4
uintptr_t fork_address_space();
// Free all frames belonging to a user address space
void paging_destroy_address_space(uintptr_t pml4_phys);
// Map a single 4 KiB virtual page in the given address space
uint8_t paging_map_page(uint64_t *pml4_phys, void *virt_addr, uintptr_t phys_addr, uint64_t flags);
// Unmap a single 4 KiB page and free its physical frame
void paging_unmap_page(uint64_t *pml4_phys, void *virt_addr);
// Create the top-level entry for the kernel-stack region
uint8_t paging_prepare_kernel_stack_region();
// Set up the initial kernel address space from the Limine responses
uint8_t paging_init(struct limine_memmap_response *memmap, struct limine_executable_address_response *exec);
