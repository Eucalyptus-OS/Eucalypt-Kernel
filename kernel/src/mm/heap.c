#include <stddef.h>
#include <stdint.h>
#include <logging/print.h>
#include <sync/spinlock.h>
#include <mm/hhdm.h>
#include <mm/page.h>
#include <mm/frame.h>
#include <mm/memory.h>
#include <mm/heap.h>

typedef struct kmalloc_header {
    size_t size;                // payload size available to the caller
    int is_free;                // 1 = free block on the free list, 0 = handed out
    struct kmalloc_header *next; // next block in the free list
    uint64_t frames;            // physical frames backing the block (1 for a normal page-sized block)
} kmalloc_header_t;

static kmalloc_header_t *free_list_head = NULL; // head of the heap's free block list
static spinlock_t heap_lock = 0;                // guards all heap book-keeping

// Allocate size bytes; small blocks come from the free list, large ones use contiguous frames
void *kmalloc(uintptr_t size) {
    if (size == 0) {
        return NULL;
    }

    size = (size + 15) & ~15UL; // round the request up to a 16-byte boundary

    uint64_t flags = spinlock_acquire_irqsave(&heap_lock);

    size_t total_needed = size + sizeof(kmalloc_header_t);
    if (total_needed > PAGE_SIZE) { // oversized requests bypass the free list entirely
        uint64_t frames = (total_needed + PAGE_SIZE - 1) / PAGE_SIZE; // pages needed to back the whole block
        uintptr_t phys = frame_alloc_contig(frames);
        if (!phys) {
            print("kmalloc: failed to allocate %d contiguous frames\n", (int)frames);
            spinlock_release_irqrestore(&heap_lock, flags);
            return NULL;
        }

        uintptr_t start = phys;
        kmalloc_header_t *chunk = (kmalloc_header_t *)phys_to_virt(start);
        memset(chunk, 0, PAGE_SIZE * frames);
        chunk->size = size;
        chunk->is_free = 0;
        chunk->next = NULL;
        chunk->frames = frames;

        void *payload = (void *)((uintptr_t)chunk + sizeof(kmalloc_header_t));
        spinlock_release_irqrestore(&heap_lock, flags);
        return payload;
    }

    kmalloc_header_t *curr = free_list_head;

    while (curr) {
        if (curr->is_free && curr->size >= size) { // reuse the first free block large enough
            if (curr->size >= size + sizeof(kmalloc_header_t) + 16) { // split only if the remainder can hold a header
                kmalloc_header_t *new_block = (kmalloc_header_t*)((uintptr_t)curr + sizeof(kmalloc_header_t) + size);
                new_block->size = curr->size - size - sizeof(kmalloc_header_t);
                new_block->is_free = 1;
                new_block->next = curr->next;
                new_block->frames = 1;

                curr->size = size;
                curr->next = new_block;
            }
            curr->is_free = 0;
            void *payload = (void*)((uintptr_t)curr + sizeof(kmalloc_header_t));
            spinlock_release_irqrestore(&heap_lock, flags);
            return payload;
        }
        curr = curr->next;
    }

    // No reusable block found: pull a fresh page and carve it into the free list
    uintptr_t phys_frame = frame_alloc(); 
    if (!phys_frame) {
        print("frame_alloc failed, out of physical memory\n");
        spinlock_release_irqrestore(&heap_lock, flags);
        return NULL;
    }

    kmalloc_header_t *new_chunk = (kmalloc_header_t*)phys_to_virt(phys_frame);
    new_chunk->size = PAGE_SIZE - sizeof(kmalloc_header_t);
    new_chunk->is_free = 0;
    new_chunk->next = NULL;
    new_chunk->frames = 1;

    new_chunk->next = free_list_head;
    free_list_head = new_chunk;

    if (new_chunk->size >= size + sizeof(kmalloc_header_t) + 16) { // split the fresh page the same way
        kmalloc_header_t *split_block = (kmalloc_header_t*)((uintptr_t)new_chunk + sizeof(kmalloc_header_t) + size);
        split_block->size = new_chunk->size - size - sizeof(kmalloc_header_t);
        split_block->is_free = 1;
        split_block->next = new_chunk->next;
        split_block->frames = 1;

        new_chunk->size = size;
        new_chunk->next = split_block;
    }

    void *payload = (void*)((uintptr_t)new_chunk + sizeof(kmalloc_header_t));
    spinlock_release_irqrestore(&heap_lock, flags);
    return payload;
}

// Free a pointer from kmalloc; multi-frame chunks go back to the frame allocator, others are coalesced
void kfree(void *addr) {
    if (!addr) {
        return;
    }

    uint64_t flags = spinlock_acquire_irqsave(&heap_lock);

    kmalloc_header_t *header = (kmalloc_header_t*)((uintptr_t)addr - sizeof(kmalloc_header_t)); // header sits just before the payload
    header->is_free = 1;

    if (header->frames > 1) { // large chunk: unlink from the list and release every frame
        kmalloc_header_t **pp = &free_list_head;
        while (*pp && *pp != header) pp = &(*pp)->next;
        if (*pp == header) *pp = header->next;

        uintptr_t phys = virt_to_phys((void *)header);
        for (uint64_t i = 0; i < header->frames; i++)
            frame_free(phys + i * PAGE_SIZE);
        spinlock_release_irqrestore(&heap_lock, flags);
        return;
    }

    kmalloc_header_t *curr = free_list_head;
    while (curr) {
        // Coalesce neighbouring free blocks when the next header lies immediately after this one
        if (curr->is_free && curr->next && curr->next->is_free) {
            uintptr_t expected_next = (uintptr_t)curr + sizeof(kmalloc_header_t) + curr->size;
            
            if (expected_next == (uintptr_t)curr->next) {
                curr->size += sizeof(kmalloc_header_t) + curr->next->size;
                curr->next = curr->next->next;
                continue; 
            }
        }
        curr = curr->next;
    }

    spinlock_release_irqrestore(&heap_lock, flags);
}
