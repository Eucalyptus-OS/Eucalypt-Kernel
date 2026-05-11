#include <limine.h>
#include <stddef.h>
#include <stdint.h>
#include <mem.h>
#include <assert.h>
#include <mm/types.h>
#include <mm/tlb.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <mm/paging.h>

extern struct limine_memmap_request memmap_request;

#define ENTRY_4K_ADDRESS_MASK   ((uint64_t)0x000FFFFFFFFFF000)
#define VADDR_TO_INDEX(VADDR, LEVEL) (((VADDR) >> (((LEVEL) - 1) * 9 + 12)) & 0x1FF)

uint64_t *kernel_pml4 = NULL;

static void map_page(uint64_t *pml4, vaddr virt, paddr phys, uint64_t flags) {
    uint64_t *current_table = pml4;

    for (int level = 4; level > 1; level--) {
        int index = VADDR_TO_INDEX(virt, level);
        uint64_t entry = __atomic_load_n(&current_table[index], __ATOMIC_SEQ_CST);
        if ((entry & ENTRY_FLAG_PRESENT) == 0) {
            uint64_t new_phys = frame_alloc();
            uint64_t *new_table = (uint64_t *)(offset + new_phys);
            memset(new_table, 0, 0x1000);

            uint64_t intermediate_flags = ENTRY_FLAG_PRESENT | ENTRY_FLAG_RW;
            if (flags & ENTRY_FLAG_USER)
                intermediate_flags |= ENTRY_FLAG_USER;

            entry = intermediate_flags | (new_phys & ENTRY_4K_ADDRESS_MASK);
            __atomic_store(&current_table[index], &entry, __ATOMIC_SEQ_CST);
        }
        entry = __atomic_load_n(&current_table[index], __ATOMIC_SEQ_CST);
        current_table = (uint64_t *)(offset + (entry & ENTRY_4K_ADDRESS_MASK));
    }

    int index = VADDR_TO_INDEX(virt, 1);
    uint64_t entry = ENTRY_FLAG_PRESENT | (phys & ENTRY_4K_ADDRESS_MASK) | flags;
    __atomic_store(&current_table[index], &entry, __ATOMIC_SEQ_CST);
}

static void unmap_page(uint64_t *pml4, vaddr virt) {
    uint64_t *current_table = pml4;

    for (int level = 4; level > 1; level--) {
        int index = VADDR_TO_INDEX(virt, level);
        uint64_t entry = __atomic_load_n(&current_table[index], __ATOMIC_SEQ_CST);
        if ((entry & ENTRY_FLAG_PRESENT) == 0)
            return;
        current_table = (uint64_t *)(offset + (entry & ENTRY_4K_ADDRESS_MASK));
    }

    int index = VADDR_TO_INDEX(virt, 1);
    uint64_t zero = 0;
    __atomic_store(&current_table[index], &zero, __ATOMIC_SEQ_CST);
}

void paging_map_page(uint64_t *pml4, vaddr virt, paddr phys, size_t length, uint64_t flags) {
    ASSERT(virt % 0x1000 == 0);
    ASSERT(phys % 0x1000 == 0);
    ASSERT(length % 0x1000 == 0);
    for (size_t i = 0; i < length; i += 0x1000)
        map_page(pml4, virt + i, phys + i, flags);
    invalidate(virt, length);
}

void paging_unmap_page(uint64_t *pml4, vaddr virt, size_t length) {
    ASSERT(virt % 0x1000 == 0);
    ASSERT(length % 0x1000 == 0);
    for (size_t i = 0; i < length; i += 0x1000)
        unmap_page(pml4, virt + i);
    invalidate(virt, length);
}

uint64_t paging_get_entry(uint64_t *pml4, vaddr virt) {
    uint64_t *current_table = pml4;

    for (int level = 4; level > 1; level--) {
        int index = VADDR_TO_INDEX(virt, level);
        uint64_t entry = __atomic_load_n(&current_table[index], __ATOMIC_SEQ_CST);
        if ((entry & ENTRY_FLAG_PRESENT) == 0)
            return 0;
        current_table = (uint64_t *)(offset + (entry & ENTRY_4K_ADDRESS_MASK));
    }

    return __atomic_load_n(&current_table[VADDR_TO_INDEX(virt, 1)], __ATOMIC_SEQ_CST);
}

paddr paging_create_pml4() {
    paddr phys = frame_alloc();
    uint64_t *pml4 = (uint64_t *)(offset + phys);
    memset(pml4, 0, 0x1000);

    for (int i = 256; i < 512; i++)
        pml4[i] = __atomic_load_n(&kernel_pml4[i], __ATOMIC_SEQ_CST);

    return phys;
}

uint64_t *paging_get_current_pml4() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)(offset + (cr3 & ENTRY_4K_ADDRESS_MASK));
}

void paging_init() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t pml4_phys = cr3 & ENTRY_4K_ADDRESS_MASK;
    uint64_t *pml4 = (uint64_t *)(offset + pml4_phys);

    kernel_pml4 = pml4;

    for (uint64_t addr = 0; addr < 0x400000; addr += 0x1000)
        map_page(pml4, addr, addr, ENTRY_FLAG_PRESENT | ENTRY_FLAG_RW);

    __asm__ volatile("mov %%cr3, %0\n\t"
                     "mov %0, %%cr3" : "=r"(cr3) :: "memory");
}