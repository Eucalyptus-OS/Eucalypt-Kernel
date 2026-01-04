#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_USER (1ULL << 2)
#define PTE_WRITE_THROUGH (1ULL << 3)
#define PTE_CACHE_DISABLE (1ULL << 4)
#define PTE_ACCESSED (1ULL << 5)
#define PTE_DIRTY (1ULL << 6)
#define PTE_HUGE (1ULL << 7)
#define PTE_GLOBAL (1ULL << 8)
#define PTE_NO_EXECUTE (1ULL << 63)

typedef uint64_t* page_table_t;

void* phys_to_virt(uint64_t phys_addr);
uint64_t virt_to_phys(void* virt_addr);
void flush_tlb();

void vmm_init();
page_table_t vmm_create_address_space();
void vmm_destroy_address_space(page_table_t pml4);
void vmm_switch_address_space(page_table_t pml4);

bool vmm_map_page(page_table_t pml4, uint64_t virt, uint64_t phys, uint64_t flags);
bool vmm_unmap_page(page_table_t pml4, uint64_t virt);
uint64_t vmm_virt_to_phys(page_table_t pml4, uint64_t virt);

bool vmm_map_range(page_table_t pml4, uint64_t virt_start, uint64_t phys_start, size_t pages, uint64_t flags);
void vmm_unmap_range(page_table_t pml4, uint64_t virt_start, size_t pages);

#endif