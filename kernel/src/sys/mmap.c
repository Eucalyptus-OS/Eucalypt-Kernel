#include <stdint.h>
#include <stddef.h>
#include <mm/page.h>
#include <mm/vmm.h>
#include <mm/heap.h>
#include <mm/hhdm.h>
#include <mm/memory.h>
#include <fs/vfs.h>
#include <multitasking/proc.h>
#include <multitasking/sched.h>
#include <mm/mmap.h>
#include <fs/devfs.h>

// Mask off the page-table-entry flags, leaving only the physical frame address
#define MMAP_ADDR_MASK 0x000FFFFFFFFFF000ULL

// Walk the 4-level page tables for va and return its mapped physical frame (0 if unmapped)
static uintptr_t mmap_page_frame(uint64_t *pml4_phys, uintptr_t va) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt((uintptr_t)pml4_phys);
    uint64_t i4 = (va >> 39) & 0x1FF;
    if (!(pml4[i4] & PAGE_PRESENT)) return 0;
    uint64_t *pml3 = (uint64_t *)phys_to_virt(pml4[i4] & MMAP_ADDR_MASK);
    uint64_t i3 = (va >> 30) & 0x1FF;
    if (!(pml3[i3] & PAGE_PRESENT)) return 0;
    uint64_t *pml2 = (uint64_t *)phys_to_virt(pml3[i3] & MMAP_ADDR_MASK);
    uint64_t i2 = (va >> 21) & 0x1FF;
    if (!(pml2[i2] & PAGE_PRESENT)) return 0;
    uint64_t *pml1 = (uint64_t *)phys_to_virt(pml2[i2] & MMAP_ADDR_MASK);
    uint64_t i1 = (va >> 12) & 0x1FF;
    uint64_t e = pml1[i1];
    if (!(e & PAGE_PRESENT)) return 0;
    return e & MMAP_ADDR_MASK;
}

// Map memory in the caller's address space: shared device mmap or anonymous zeroed pages
intptr_t sys_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, off_t offset) {
    struct pcb *p = sched_current_proc();
    if (!p) return -1;
    if (len == 0) return -EINVAL;
    // MAP_FIXED (force placement at exactly addr) is not supported yet
    if (flags & MAP_FIXED) return -EINVAL;

    // Device-backed path: MAP_SHARED against a devfs fd (e.g. the framebuffer)
    if ((flags & MAP_SHARED) && !(flags & MAP_ANONYMOUS) && fd >= 0) {
        size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
        size_t map_len = pages * PAGE_SIZE;

        uintptr_t va;
        // Round the hint up to a page; with no hint, keep allocating from the mmap cursor
        if (addr != 0) {
            va = (addr + PAGE_SIZE - 1) & ~((uintptr_t)PAGE_SIZE - 1);
        } else {
            va = p->mmap_cursor;
        }

        if (!p->fd_table[fd]) return -EINVAL;
        vfs_node_t *node = p->fd_table[fd]->node;
        if (!node || node->type != VFS_NODE_DEV || !node->priv) return -EINVAL;

        devfs_dev_t *dev = (devfs_dev_t *)node->priv;
        fb_info_t *fb = (fb_info_t *)dev->priv;
        if (fb->phys == 0 || map_len > fb->size) return -EINVAL;

        // Permissions: always present+user, writable only when requested
        uint64_t f = PAGE_PRESENT | PAGE_USER;
        if (prot & PROT_WRITE) f |= PAGE_WRITABLE;

        // Identity-map the device's physical frame range into the user's space
        for (size_t i = 0; i < pages; i++) {
            uintptr_t pa = fb->phys + i * PAGE_SIZE;
            paging_map_page((uint64_t *)p->addr_space, (void *)(va + i * PAGE_SIZE), pa, f);
        }

        // Record the mapping in the process's region list for later munmap/mprotect
        struct mmap_region *r = (struct mmap_region *)kmalloc(sizeof(struct mmap_region));
        if (!r) return -ENOMEM;
        r->base = va;
        r->len = map_len;
        r->prot = (uint32_t)prot;
        r->next = p->mmaps;
        p->mmaps = r;

        // Bump the cursor past our mapping when the kernel picked the address
        if (addr == 0) {
            p->mmap_cursor = va + map_len;
        }
        return (intptr_t)va;
    }

    // Only anonymous mappings are supported after the device path above
    if (!(flags & MAP_ANONYMOUS)) return -EINVAL;

    size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t map_len = pages * PAGE_SIZE;

    uintptr_t va;
    // Round the hint up to a page; with no hint, keep allocating from the mmap cursor
    if (addr != 0) {
        va = (addr + PAGE_SIZE - 1) & ~((uintptr_t)PAGE_SIZE - 1);
    } else {
        va = p->mmap_cursor;
    }

    uint64_t f = PAGE_PRESENT | PAGE_USER;
    if (prot & PROT_WRITE) f |= PAGE_WRITABLE;

    // Map fresh pages and zero them so the region reads as clean anonymous memory
    if (!vmm_map_region((uint64_t *)p->addr_space, (void *)va, f, (int)pages)) {
        return -ENOMEM;
    }

    memset((void *)va, 0, map_len);

    // Keep the region on the process list for later munmap/mprotect
    struct mmap_region *r = (struct mmap_region *)kmalloc(sizeof(struct mmap_region));
    if (!r) {
        return -ENOMEM;
    }
    r->base = va;
    r->len = map_len;
    r->prot = (uint32_t)prot;
    r->next = p->mmaps;
    p->mmaps = r;

    // Bump the cursor past our mapping when the kernel picked the address
    if (addr == 0) {
        p->mmap_cursor = va + map_len;
    }

    (void)fd;
    (void)offset;
    return (intptr_t)va;
}

// Find the mapping that fully covers [addr, addr+len) after page rounding; NULL if none
static struct mmap_region *find_region(struct pcb *p, uintptr_t addr, size_t len) {
    // Round the requested range to page boundaries before the containment check
    uintptr_t start = addr & ~((uintptr_t)PAGE_SIZE - 1);
    uintptr_t end = ((addr + len + PAGE_SIZE - 1) & ~((uintptr_t)PAGE_SIZE - 1));
    for (struct mmap_region *r = p->mmaps; r; r = r->next) {
        if (r->base <= start && r->base + r->len >= end) {
            return r;
        }
    }
    return NULL;
}

// Unmap a page-aligned region; only a mapping starting exactly at `addr` is removed
int sys_munmap(uintptr_t addr, size_t len) {
    struct pcb *p = sched_current_proc();
    if (!p) return -EINVAL;
    if (len == 0) return -EINVAL;

    // Page-align the range; the region list is always page-granular
    uintptr_t start = addr & ~((uintptr_t)PAGE_SIZE - 1);
    size_t map_len = ((addr + len + PAGE_SIZE - 1) & ~((uintptr_t)PAGE_SIZE - 1)) - start;

    struct mmap_region *prev = NULL;
    struct mmap_region *r = p->mmaps;
    while (r) {
        // Only whole regions anchored at `start` are freed (no partial unmapping)
        if (r->base == start) {
            // Drop each page mapping from the address space
            for (uintptr_t va = start; va < start + map_len; va += PAGE_SIZE) {
                paging_unmap_page((uint64_t *)p->addr_space, (void *)va);
            }
            // Splice the region out of the per-process list and free its bookkeeping
            if (prev) prev->next = r->next;
            else p->mmaps = r->next;
            kfree(r);
            return 0;
        }
        prev = r;
        r = r->next;
    }
    return -EINVAL;
}

// Change the protection bits on an existing mapping (must be a single whole region)
int sys_mprotect(uintptr_t addr, size_t len, int prot) {
    struct pcb *p = sched_current_proc();
    if (!p) return -EINVAL;
    if (len == 0) return -EINVAL;

    uintptr_t start = addr & ~((uintptr_t)PAGE_SIZE - 1);
    uintptr_t end = (addr + len + PAGE_SIZE - 1) & ~((uintptr_t)PAGE_SIZE - 1);

    // Refuse ranges not fully backed by one mapping
    struct mmap_region *r = find_region(p, start, end - start);
    if (!r) return -EINVAL;

    // Permissions: present+user always, writable only if requested
    uint64_t f = PAGE_PRESENT | PAGE_USER;
    if (prot & PROT_WRITE) f |= PAGE_WRITABLE;

    // Re-map every present frame with the new flags (no-op for pages still unmapped)
    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        uintptr_t frame = mmap_page_frame((uint64_t *)p->addr_space, va);
        if (frame) {
            paging_map_page((uint64_t *)p->addr_space, (void *)va, frame, f);
        }
    }

    r->prot = (uint32_t)prot;
    return 0;
}
