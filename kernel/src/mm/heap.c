#include <stddef.h>
#include <stdint.h>
#include <memory.h>
#include <mm/heap.h>
#include <mm/paging.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <sync/spinlock.h>
#include <multitasking/sched.h>
#include <logging/print.h>

// Every allocation is preceded by a header; the free list is kept sorted by
// address (growth always appends above existing blocks, splits stay in place),
// which makes coalescing in kfree a single pass.
struct block_header {
    uint64_t size;             // payload bytes, excluding this header
    uint64_t is_free;
    uint64_t _pad;             // header must stay a multiple of 16 so
                               // payloads are 16-byte aligned (FXSAVE et al.)
    struct block_header *next;
};

static struct block_header *free_head = NULL;
static size_t mapped_pages = 0;
static spinlock_t heap_lock = 0;

static uint8_t heap_grow(size_t pages) {
    if (mapped_pages + pages > HEAP_MAX_PAGES) {
        return 1;
    }

    void *bump = (void *)(HEAP_VIRT_BASE + mapped_pages * PAGE_SIZE);
    // pml4 stored by paging.c is physical — wrap it for map_page().
    uint64_t *pml4 = (uint64_t *)phys_to_virt((uintptr_t)kernel_pml4);

    for (size_t i = 0; i < pages; i++) {
        uintptr_t frame = frame_alloc();
        if (!frame) {
            return 1;
        }
        if (map_page(pml4, (uint8_t *)bump + i * PAGE_SIZE, frame,
                     PAGE_PRESENT | PAGE_WRITABLE | PAGE_NXE)) {
            frame_free(frame);
            return 1;
        }
    }

    mapped_pages += pages;
    return 0;
}

static struct block_header *heap_tail() {
    struct block_header *curr = free_head;
    while (curr && curr->next) {
        curr = curr->next;
    }
    return curr;
}

static void heap_append(struct block_header *block) {
    struct block_header *tail = heap_tail();
    if (tail) {
        tail->next = block;
    } else {
        free_head = block;
    }
    block->next = NULL;
}

uint8_t heap_init() {
    if (heap_grow(16)) {
        return 1;
    }

    // One big free block covering everything just mapped.
    struct block_header *block = (struct block_header *)HEAP_VIRT_BASE;
    block->size = mapped_pages * PAGE_SIZE - sizeof(struct block_header);
    block->is_free = 1;
    block->next = NULL;
    free_head = block;

    print("Heap initialized: %u pages at 0x%lx",
          (unsigned)mapped_pages, HEAP_VIRT_BASE);
    return 0;
}

void *kmalloc(uintptr_t size) {
    if (!size) {
        return NULL;
    }

    // Keep payloads 16-byte aligned: headers are a multiple of 16 on x86_64.
    size = (size + 15) & ~15UL;

    preempt_disable();
    uint64_t flags = spinlock_acquire_irqsave(&heap_lock);

    size_t total_needed = size + sizeof(struct block_header);

    // First fit over the free list.
    struct block_header *curr = free_head;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            break;
        }
        curr = curr->next;
    }

    if (!curr) {
        // Nothing fits: grow the region and carve the new block off the bump.
        size_t pages = (total_needed + PAGE_SIZE - 1) / PAGE_SIZE;
        if (heap_grow(pages)) {
            spinlock_release_irqrestore(&heap_lock, flags);
            return NULL;
        }

        struct block_header *block =
            (struct block_header *)(HEAP_VIRT_BASE + (mapped_pages - pages) * PAGE_SIZE);
        size_t span = pages * PAGE_SIZE - sizeof(struct block_header);

        // If the leftover after the request can hold another block, split it
        // into a trailing free block; otherwise hand the whole span over so
        // internal fragmentation stays bounded to one header's slack.
        if (span >= size + sizeof(struct block_header) + 16) {
            block->size = size;
            block->is_free = 0;

            struct block_header *rest =
                (struct block_header *)((uintptr_t)block + sizeof(struct block_header) + size);
            rest->size = span - size - sizeof(struct block_header);
            rest->is_free = 1;
            rest->next = NULL;

            heap_append(block);     // block first...
            heap_append(rest);      // ...then rest keeps list address-sorted
        } else {
            block->size = span;
            block->is_free = 0;
            heap_append(block);
        }

        curr = block;
    } else if (curr->size >= size + sizeof(struct block_header) + 16) {
        // Split in place: shrink the hit, insert a free remainder after it.
        struct block_header *rest =
            (struct block_header *)((uintptr_t)curr + sizeof(struct block_header) + size);
        rest->size = curr->size - size - sizeof(struct block_header);
        rest->is_free = 1;
        rest->next = curr->next;
        curr->next = rest;
        curr->size = size;
    }

    curr->is_free = 0;
    void *payload = (void *)((uintptr_t)curr + sizeof(struct block_header));
    spinlock_release_irqrestore(&heap_lock, flags);
    preempt_enable();
    return payload;
}

void kfree(void *addr) {
    if (!addr) {
        return;
    }

    preempt_disable();
    uint64_t flags = spinlock_acquire_irqsave(&heap_lock);

    struct block_header *header = (struct block_header *)((uintptr_t)addr - sizeof(struct block_header));
    header->is_free = 1;

    // Single coalescing pass: merge any run of adjacent-by-address free blocks.
    struct block_header *curr = free_head;
    while (curr && curr->next) {
        if (curr->is_free && curr->next->is_free &&
            (uintptr_t)curr + sizeof(struct block_header) + curr->size ==
                (uintptr_t)curr->next) {
            curr->size += sizeof(struct block_header) + curr->next->size;
            curr->next = curr->next->next;
            continue;   // re-check same block against its new successor
        }
        curr = curr->next;
    }

    spinlock_release_irqrestore(&heap_lock, flags);
    preempt_enable();
}
