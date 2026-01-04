#include <x86_64/memory/vmm.h>
#include <x86_64/memory/pmm.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_kernel_address_request kernel_address_request;

#define PAGE_SIZE 4096
#define ENTRIES_PER_TABLE 512

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

static page_table_t kernel_pml4;

void* phys_to_virt(uint64_t phys_addr) {
    return (void*)(phys_addr + hhdm_request.response->offset);
}

uint64_t virt_to_phys(void* virt_addr) {
    return (uint64_t)virt_addr - hhdm_request.response->offset;
}

void flush_tlb() {
    asm volatile (
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %%cr3\n\t"
        ::: "rax"
    );
}

static void invlpg(void* addr) {
    asm volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

static page_table_t get_or_create_table(page_table_t table, size_t index, uint64_t flags) {
    if (table[index] & PTE_PRESENT) {
        return (page_table_t)phys_to_virt(table[index] & 0x000FFFFFFFFFF000);
    }
    
    void* new_table_phys = pmm_alloc();
    if (!new_table_phys) return NULL;
    
    page_table_t new_table = (page_table_t)phys_to_virt((uint64_t)new_table_phys);
    for (size_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        new_table[i] = 0;
    }
    
    table[index] = (uint64_t)new_table_phys | flags | PTE_PRESENT;
    return new_table;
}

page_table_t vmm_create_address_space() {
    void* pml4_phys = pmm_alloc();
    if (!pml4_phys) return NULL;
    
    page_table_t pml4 = (page_table_t)phys_to_virt((uint64_t)pml4_phys);
    for (size_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        pml4[i] = 0;
    }
    
    for (size_t i = 256; i < ENTRIES_PER_TABLE; i++) {
        pml4[i] = kernel_pml4[i];
    }
    
    return pml4;
}

void vmm_destroy_address_space(page_table_t pml4) {
    if (!pml4) return;
    
    for (size_t pml4e = 0; pml4e < 256; pml4e++) {
        if (!(pml4[pml4e] & PTE_PRESENT)) continue;
        
        page_table_t pdpt = (page_table_t)phys_to_virt(pml4[pml4e] & 0x000FFFFFFFFFF000);
        for (size_t pdpte = 0; pdpte < ENTRIES_PER_TABLE; pdpte++) {
            if (!(pdpt[pdpte] & PTE_PRESENT)) continue;
            
            page_table_t pd = (page_table_t)phys_to_virt(pdpt[pdpte] & 0x000FFFFFFFFFF000);
            for (size_t pde = 0; pde < ENTRIES_PER_TABLE; pde++) {
                if (!(pd[pde] & PTE_PRESENT)) continue;
                
                pmm_free((void*)(pd[pde] & 0x000FFFFFFFFFF000));
            }
            pmm_free((void*)(pdpt[pdpte] & 0x000FFFFFFFFFF000));
        }
        pmm_free((void*)(pml4[pml4e] & 0x000FFFFFFFFFF000));
    }
    
    pmm_free((void*)virt_to_phys(pml4));
}

void vmm_switch_address_space(page_table_t pml4) {
    if (!pml4) return;
    uint64_t pml4_phys = virt_to_phys(pml4);
    asm volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
}

bool vmm_map_page(page_table_t pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    size_t pml4e = (virt >> 39) & 0x1FF;
    size_t pdpte = (virt >> 30) & 0x1FF;
    size_t pde = (virt >> 21) & 0x1FF;
    size_t pte = (virt >> 12) & 0x1FF;
    
    page_table_t pdpt = get_or_create_table(pml4, pml4e, PTE_WRITABLE | PTE_USER);
    if (!pdpt) return false;
    
    page_table_t pd = get_or_create_table(pdpt, pdpte, PTE_WRITABLE | PTE_USER);
    if (!pd) return false;
    
    page_table_t pt = get_or_create_table(pd, pde, PTE_WRITABLE | PTE_USER);
    if (!pt) return false;
    
    pt[pte] = phys | flags | PTE_PRESENT;
    invlpg((void*)virt);
    
    return true;
}

bool vmm_unmap_page(page_table_t pml4, uint64_t virt) {
    size_t pml4e = (virt >> 39) & 0x1FF;
    size_t pdpte = (virt >> 30) & 0x1FF;
    size_t pde = (virt >> 21) & 0x1FF;
    size_t pte = (virt >> 12) & 0x1FF;
    
    if (!(pml4[pml4e] & PTE_PRESENT)) return false;
    page_table_t pdpt = (page_table_t)phys_to_virt(pml4[pml4e] & 0x000FFFFFFFFFF000);
    
    if (!(pdpt[pdpte] & PTE_PRESENT)) return false;
    page_table_t pd = (page_table_t)phys_to_virt(pdpt[pdpte] & 0x000FFFFFFFFFF000);
    
    if (!(pd[pde] & PTE_PRESENT)) return false;
    page_table_t pt = (page_table_t)phys_to_virt(pd[pde] & 0x000FFFFFFFFFF000);
    
    if (!(pt[pte] & PTE_PRESENT)) return false;
    
    pt[pte] = 0;
    invlpg((void*)virt);
    
    return true;
}

uint64_t vmm_virt_to_phys(page_table_t pml4, uint64_t virt) {
    size_t pml4e = (virt >> 39) & 0x1FF;
    size_t pdpte = (virt >> 30) & 0x1FF;
    size_t pde = (virt >> 21) & 0x1FF;
    size_t pte = (virt >> 12) & 0x1FF;
    
    if (!(pml4[pml4e] & PTE_PRESENT)) return 0;
    page_table_t pdpt = (page_table_t)phys_to_virt(pml4[pml4e] & 0x000FFFFFFFFFF000);
    
    if (!(pdpt[pdpte] & PTE_PRESENT)) return 0;
    page_table_t pd = (page_table_t)phys_to_virt(pdpt[pdpte] & 0x000FFFFFFFFFF000);
    
    if (!(pd[pde] & PTE_PRESENT)) return 0;
    page_table_t pt = (page_table_t)phys_to_virt(pd[pde] & 0x000FFFFFFFFFF000);
    
    if (!(pt[pte] & PTE_PRESENT)) return 0;
    
    return (pt[pte] & 0x000FFFFFFFFFF000) | (virt & 0xFFF);
}

bool vmm_map_range(page_table_t pml4, uint64_t virt_start, uint64_t phys_start, size_t pages, uint64_t flags) {
    for (size_t i = 0; i < pages; i++) {
        if (!vmm_map_page(pml4, virt_start + i * PAGE_SIZE, phys_start + i * PAGE_SIZE, flags)) {
            for (size_t j = 0; j < i; j++) {
                vmm_unmap_page(pml4, virt_start + j * PAGE_SIZE);
            }
            return false;
        }
    }
    return true;
}

void vmm_unmap_range(page_table_t pml4, uint64_t virt_start, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        vmm_unmap_page(pml4, virt_start + i * PAGE_SIZE);
    }
}

void vmm_init() {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    kernel_pml4 = (page_table_t)phys_to_virt(cr3);
}