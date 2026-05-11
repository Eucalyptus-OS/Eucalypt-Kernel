#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <mm/types.h>

#define PML4_VIRT(space) ((uint64_t *)(offset + (space)->pml4))

struct vm_region {
    uint64_t         base;
    size_t           size;
    struct vm_region *next;
};

struct vm_space {
    paddr          pml4;
    uint64_t          heap_base;
    uint64_t          heap_end;
    uint64_t          next_free;
    struct vm_region *regions;
};

#define VM_READ     (1 << 0)
#define VM_WRITE    (1 << 1)
#define VM_EXEC     (1 << 2)
#define VM_USER     (1 << 3)
#define VM_UNCACHED (1 << 4)

struct vm_space *vm_space_create(uint64_t heap_base, size_t heap_size);
void vm_space_destroy(struct vm_space *space);
void vm_switch(struct vm_space *space);
uint64_t vm_alloc(struct vm_space *space, size_t size, uint64_t flags);
uint64_t vm_alloc_at(struct vm_space *space, uint64_t vaddr, size_t size, uint64_t flags);
uint64_t vm_map(struct vm_space *space, uint64_t paddr, size_t size, uint64_t flags);
uint64_t vm_map_at(struct vm_space *space, uint64_t vaddr, uint64_t paddr, size_t size, uint64_t flags);
uint64_t vm_map_region(struct vm_space *space, struct vm_region *region, uint64_t flags);
void vm_free(struct vm_space *space, uint64_t vaddr, size_t size);
uint64_t vm_virt_to_phys(struct vm_space *space, uint64_t vaddr);
bool vm_is_mapped(struct vm_space *space, uint64_t vaddr);