#include <stddef.h>
#include <stdint.h>
#include <mem.h>
#include <assert.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <mm/paging.h>
#include <mm/vm.h>

struct vm_space *vm_space_create(uint64_t heap_base, size_t heap_size) {
    uint64_t phys = frame_alloc();
    struct vm_space *space = (struct vm_space *)(offset + phys);
    space->pml4 = paging_create_pml4();
    space->heap_base = heap_base;
    space->heap_end = heap_base + heap_size;
    space->next_free = heap_base;
    space->regions = NULL;
    return space;
}

static struct vm_region *region_alloc(uint64_t base, size_t size) {
    uint64_t phys = frame_alloc();
    struct vm_region *r = (struct vm_region *)(offset + phys);
    r->base = base;
    r->size = size;
    r->next = NULL;
    return r;
}

static void region_insert(struct vm_space *space, struct vm_region *r) {
    r->next = space->regions;
    space->regions = r;
}

static struct vm_region *region_remove(struct vm_space *space, uint64_t base) {
    struct vm_region **cur = &space->regions;
    while (*cur) {
        if ((*cur)->base == base) {
            struct vm_region *found = *cur;
            *cur = found->next;
            return found;
        }
        cur = &(*cur)->next;
    }
    return NULL;
}

uint64_t vm_alloc(struct vm_space *space, size_t size, uint64_t flags) {
    ASSERT_NOT_NULL(space);
    ASSERT(size > 0);

    size = (size + 0xFFF) & ~(size_t)0xFFF;

    ASSERT(space->next_free + size <= space->heap_end);

    uint64_t vaddr = space->next_free;

    for (size_t i = 0; i < size; i += 0x1000) {
        uint64_t phys = frame_alloc();
        paging_map_page(space->pml4, vaddr + i, phys, 0x1000, flags);
    }

    struct vm_region *r = region_alloc(vaddr, size);
    region_insert(space, r);

    space->next_free += size;

    return vaddr;
}

uint64_t vm_alloc_at(struct vm_space *space, uint64_t vaddr, size_t size, uint64_t flags) {
    ASSERT_NOT_NULL(space);
    ASSERT(vaddr % 0x1000 == 0);
    ASSERT(size > 0);

    size = (size + 0xFFF) & ~(size_t)0xFFF;

    for (size_t i = 0; i < size; i += 0x1000) {
        uint64_t phys = frame_alloc();
        paging_map_page(space->pml4, vaddr + i, phys, 0x1000, flags);
    }

    struct vm_region *r = region_alloc(vaddr, size);
    region_insert(space, r);

    return vaddr;
}

uint64_t vm_map(struct vm_space *space, uint64_t paddr, size_t size, uint64_t flags) {
    ASSERT_NOT_NULL(space);
    ASSERT(paddr % 0x1000 == 0);
    ASSERT(size > 0);

    size = (size + 0xFFF) & ~(size_t)0xFFF;

    ASSERT(space->next_free + size <= space->heap_end);

    uint64_t vaddr = space->next_free;

    paging_map_page(space->pml4, vaddr, paddr, size, flags);

    struct vm_region *r = region_alloc(vaddr, size);
    region_insert(space, r);

    space->next_free += size;

    return vaddr;
}

uint64_t vm_map_at(struct vm_space *space, uint64_t vaddr, uint64_t paddr, size_t size, uint64_t flags) {
    ASSERT_NOT_NULL(space);
    ASSERT(vaddr % 0x1000 == 0);
    ASSERT(paddr % 0x1000 == 0);
    ASSERT(size > 0);

    size = (size + 0xFFF) & ~(size_t)0xFFF;

    paging_map_page(space->pml4, vaddr, paddr, size, flags);

    struct vm_region *r = region_alloc(vaddr, size);
    region_insert(space, r);

    return vaddr;
}

uint64_t vm_map_region(struct vm_space *space, struct vm_region *region, uint64_t flags) {
    ASSERT_NOT_NULL(space);
    ASSERT_NOT_NULL(region);
    ASSERT(region->base % 0x1000 == 0);
    ASSERT(region->size > 0);

    size_t size = (region->size + 0xFFF) & ~(size_t)0xFFF;

    ASSERT(space->next_free + size <= space->heap_end);

    uint64_t vaddr = space->next_free;

    paging_map_page(space->pml4, vaddr, region->base, size, flags);

    region->size = size;
    region_insert(space, region);

    space->next_free += size;

    return vaddr;
}

void vm_free(struct vm_space *space, uint64_t vaddr, size_t size) {
    ASSERT_NOT_NULL(space);
    ASSERT(vaddr % 0x1000 == 0);
    ASSERT(size > 0);

    size = (size + 0xFFF) & ~(size_t)0xFFF;

    for (size_t i = 0; i < size; i += 0x1000) {
        uint64_t entry = paging_get_entry(space->pml4, vaddr + i);
        if (entry & ENTRY_FLAG_PRESENT)
            frame_free(entry & ENTRY_4K_ADDRESS_MASK);
    }

    paging_unmap_page(space->pml4, vaddr, size);

    struct vm_region *r = region_remove(space, vaddr);
    if (r)
        frame_free((uint64_t)r - offset);
}

void vm_switch(struct vm_space *space) {
    ASSERT_NOT_NULL(space);
    uint64_t pml4_phys = (uint64_t)space->pml4 - offset;
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

uint64_t vm_virt_to_phys(struct vm_space *space, uint64_t vaddr) {
    ASSERT_NOT_NULL(space);
    uint64_t entry = paging_get_entry(space->pml4, vaddr);
    ASSERT(entry & ENTRY_FLAG_PRESENT);
    return (entry & ENTRY_4K_ADDRESS_MASK) | (vaddr & 0xFFF);
}

void vm_space_destroy(struct vm_space *space) {
    ASSERT_NOT_NULL(space);
    struct vm_region *r = space->regions;
    while (r) {
        struct vm_region *next = r->next;
        vm_free(space, r->base, r->size);
        r = next;
    }
}