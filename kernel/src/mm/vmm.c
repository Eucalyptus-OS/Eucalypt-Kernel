#include <stdint.h>
#include <stddef.h>
#include <memory.h>
#include <mm/paging.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <mm/vmm.h>
#include <mm/heap.h>
#include <logging/print.h>

#define ENTRIES_PER_TABLE 512
#define PTE_PHYS_MASK 0x000FFFFFFFFFF000ULL

// The kernel's own space: high-half mappings created by paging_init, plus
// every kernel-side vmm_map_at (thread stacks, future kernel allocations).
struct vmm_space kernel_space;

static inline uint64_t level_of(void *v, uint8_t level) {
    switch (level) {
    case 4:
        return ((uint64_t)v >> 39) & 0x1FF;
    case 3:
        return ((uint64_t)v >> 30) & 0x1FF;
    case 2:
        return ((uint64_t)v >> 21) & 0x1FF;
    case 1:
        return ((uint64_t)v >> 12) & 0x1FF;
    }
    return 0;
}

void *vmm_map_at(struct vmm_space *space, void *vaddr, uint64_t flags, int pages) {
    if (!space || !vaddr || pages <= 0) {
        print("vmm_map_at: bad args space=%lx vaddr=%lx pages=%d",
              (uintptr_t)space, (uintptr_t)vaddr, pages);
        return NULL;
    }

    struct vm_region *region = kmalloc(sizeof(struct vm_region));
    if (!region) {
        print("vmm_map_at: kmalloc failed for region");
        return NULL;
    }

    region->base = (uint64_t)vaddr;
    region->end = 0;
    region->len = 0;
    region->flags = flags;

    // map_page walks tables through the HHDM; the stored root is physical.
    uint64_t *pml4 = phys_to_virt((uintptr_t)space->pml4);

    for (int i = 0; i < pages; i++) {
        void *page_vaddr = (void *)(region->base + (uint64_t)i * PAGE_SIZE);

        uintptr_t frame = frame_alloc();
        if (!frame) {
            print("vmm_map_at: frame_alloc failed at page %d/%d", i, pages);
            goto fail;
        }

        if (map_page(pml4, page_vaddr, frame, flags)) {
            print("vmm_map_at: map_page failed vaddr=%lx", (uintptr_t)page_vaddr);
            frame_free(frame);
            goto fail;
        }

        region->len++;
    }

    region->end = region->base + region->len * PAGE_SIZE;

    list_push_back(&space->regions, &region->link);
    return (void *)region->base;

fail:
    for (uint64_t j = 0; j < region->len; j++) {
        void *page_vaddr = (void *)(region->base + j * PAGE_SIZE);
        free_page(pml4, page_vaddr); // releases each backing frame too
    }
    kfree(region);
    return NULL;
}

struct vm_region *vmm_find_region(struct vmm_space *space, uint64_t base) {
    list_foreach(&space->regions, n) {
        struct vm_region *r = container_of(n, struct vm_region, link);
        if (r->base == base) {
            return r;
        }
    }
    return NULL;
}

void vmm_free_region(struct vmm_space *space, struct vm_region *region) {
    if (!space || !region) {
        return;
    }

    uint64_t *pml4 = phys_to_virt((uintptr_t)space->pml4);
    for (uint64_t va = region->base; va != region->end; va += PAGE_SIZE) {
        free_page(pml4, (void *)va);
    }

    list_remove(&space->regions, &region->link);
    kfree(region);
}

// Until ring 3 exists there are no private mappings, so creation shares the
// whole root verbatim: the kernel runs identity-mapped in the low half and
// keeps its HHDM/stack mappings in the high half, and both must stay visible
// in every address space. When user mode lands, this narrows to copying only
// kernel-owned ranges above USER_VA_LIMIT, and free_lower_tables becomes
// safe to call here.
uint8_t vmm_create_space_into(struct vmm_space *space) {
    uintptr_t pml4_phys = frame_alloc();
    if (!pml4_phys) {
        print("vmm_create_space_into: no frame for PML4");
        return 1;
    }

    memset(phys_to_virt(pml4_phys), 0, PAGE_SIZE);

    // Until ring 3 exists there are no private mappings, so share the whole
    // root verbatim: the kernel runs identity-mapped in the low half and
    // keeps its HHDM/stack mappings in the high half, and both must stay
    // visible in every address space. When user mode lands, this narrows to
    // copying only the kernel-owned ranges above USER_VA_LIMIT.
    uint64_t *dst = phys_to_virt(pml4_phys);
    uint64_t *src = phys_to_virt((uintptr_t)kernel_pml4);
    memcpy(dst, src, ENTRIES_PER_TABLE * sizeof(uint64_t));

    space->pml4 = (uint64_t *)pml4_phys;
    list_init(&space->regions);
    return 0;
}

void vmm_destroy_space(struct vmm_space *space) {
    if (!space || space == &kernel_space) {
        return;
    }

    while (!list_empty(&space->regions)) {
        struct vm_region *r = container_of(space->regions.head,
                                           struct vm_region, link);
        vmm_free_region(space, r);
    }

    // Every PML4 entry is currently shared with kernel_space (see
    // vmm_create_space_into), so only the root frame itself is per-space.
    // free_lower_tables must not run until spaces own private entries.
    frame_free((uintptr_t)space->pml4);
}

uint8_t vmm_init() {
    // Bind the kernel space to the root paging_init built.
    kernel_space.pml4 = kernel_pml4;
    list_init(&kernel_space.regions);
    return 0;
}
