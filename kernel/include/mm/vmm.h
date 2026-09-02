#pragma once

#include <stdint.h>

#include <lib/list.h>

// User (private) mappings must stay below this line; everything above it is
// owned by kernel_space and shared into every address space via copied
// high-half PML4 entries.
#define USER_VA_LIMIT 0x0000800000000000ULL

struct vm_region {
    uint64_t base;
    uint64_t end;
    uint64_t len;
    uint64_t flags;
    struct list_node link;
};

// One address space: a physical PML4 root plus the bookkeeping for its
// private (low-half) mappings. High-half entries are shared with the kernel
// and therefore tracked by kernel_space only.
struct vmm_space {
    uint64_t *pml4;      // physical root, CR3-ready
    struct list regions; // private low-half mappings
};

extern struct vmm_space kernel_space;

uint8_t vmm_init();

// Map `pages` at the exact virtual address requested. Returns the base on
// success, NULL on failure (with any partial mapping unwound).
void *vmm_map_at(struct vmm_space *space, void *vaddr, uint64_t flags, int pages);
struct vm_region *vmm_find_region(struct vmm_space *space, uint64_t base);
void vmm_free_region(struct vmm_space *space, struct vm_region *region);

// Fresh space sharing the kernel's high half, initialized in place (PCBs
// embed their space). Returns nonzero on failure; destroy frees all private
// mappings and the low-half page tables. Never call these on kernel_space.
uint8_t vmm_create_space_into(struct vmm_space *space);
void vmm_destroy_space(struct vmm_space *space);
