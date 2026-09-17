#include <stdint.h>
#include <stddef.h>
#include <logging/print.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <mm/page.h>
#include <mm/vmm.h>

static linked_list_t list;

void *vmm_map_region(uint64_t *pml4_phys, void *vaddr, uint64_t flags, int pages_needed) {
    if (!pml4_phys || !vaddr || pages_needed <= 0) {
        print("vmm_map_region: bad args pml4=%X vaddr=%X pages=%d\n", pml4_phys, vaddr, pages_needed);
        return NULL;
    }

    uintptr_t node_frame = frame_alloc();
    if (!node_frame) {
        print("vmm_map_region: frame_alloc failed for node\n");
        return NULL;
    }

    linked_list_node_t *node = (linked_list_node_t *)phys_to_virt(node_frame);
    node->base  = (uint64_t)vaddr;
    node->len   = 0;
    node->flags = flags;
    node->next  = NULL;
    node->prev  = NULL;

    for (int i = 0; i < pages_needed; i++) {
        void *page_vaddr = (void *)((uint64_t)vaddr + (uint64_t)i * PAGE_SIZE);

        uintptr_t frame = frame_alloc();
        if (!frame) {
            print("vmm_map_region: frame_alloc failed at page %d/%d\n", i, pages_needed);
            goto fail;
        }

        uint8_t ok = paging_map_page(pml4_phys, page_vaddr, frame, flags);
        if (ok) {
            print("vmm_map_region: paging_map_page failed vaddr=%X frame=0x%lx\n", page_vaddr, (unsigned long)frame);
            frame_free(frame);
            goto fail;
        }

        node->len++;
    }

    node->end = (uint64_t)vaddr + node->len * PAGE_SIZE;

    node->prev = list.tail;
    if (list.tail) {
        list.tail->next = node;
    } else {
        list.head = node;
    }
    list.tail = node;
    list.count++;

    return (void *)node->base;

fail:
    for (uint64_t j = 0; j < node->len; j++) {
        void *page_vaddr = (void *)((uint64_t)vaddr + j * PAGE_SIZE);
        paging_unmap_page(pml4_phys, page_vaddr);
    }
    frame_free(node_frame);
    return NULL;
}

void vmm_free_region(uint64_t *pml4, linked_list_node_t *node) {
    if (!pml4 || !node) {
        return;
    }

    uint64_t base = node->base;
    uint64_t end  = node->end;

    for (uint64_t i = base; i != end; i += PAGE_SIZE) {
        paging_unmap_page(pml4, (void *)i);
    }

    if (node->prev) {
        node->prev->next = node->next;
    } else {
        list.head = node->next;
    }

    if (node->next) {
        node->next->prev = node->prev;
    } else {
        list.tail = node->prev;
    }

    list.count--;

    frame_free((uintptr_t)virt_to_phys((void *)node));
}

linked_list_node_t *vmm_find_region(uint64_t vaddr) {
    for (linked_list_node_t *n = list.head; n; n = n->next) {
        if (n->base == vaddr) {
            return n;
        }
    }
    return NULL;
}

uint8_t vmm_init() {
    list.count = 0;
    list.head  = NULL;
    list.tail  = NULL;
    return 0;
}