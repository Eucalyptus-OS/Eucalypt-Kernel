#include <stddef.h>
#include <stdint.h>
#include <mem.h>
#include <mm/hhdm.h>
#include <mm/frame.h>
#include <mm/paging.h>
#include <mm/heap.h>

#define HEAP_START  0xFFFF900000000000
#define HEAP_MAGIC  0xDEADBEEFCAFEBABE

typedef struct block {
    uint64_t magic;
    size_t size;
    struct block *next;
} block_t;

static block_t *free_list = NULL;
static uint64_t heap_top = HEAP_START;

static void heap_expand(size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = frame_alloc();
        paging_map_page(kernel_pml4, heap_top, phys, 0x1000, ENTRY_FLAG_PRESENT | ENTRY_FLAG_RW);
        block_t *blk = (block_t *)heap_top;
        blk->magic = HEAP_MAGIC;
        blk->size = 0x1000 - sizeof(block_t);
        blk->next = free_list;
        free_list = blk;
        heap_top += 0x1000;
    }
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;

    size = (size + 15) & ~(size_t)15;

    block_t *prev = NULL;
    block_t *cur = free_list;

    while (cur) {
        if (cur->size >= size) {
            if (cur->size >= size + sizeof(block_t) + 16) {
                block_t *split = (block_t *)((uint8_t *)cur + sizeof(block_t) + size);
                split->magic = HEAP_MAGIC;
                split->size = cur->size - size - sizeof(block_t);
                split->next = cur->next;
                cur->size = size;
                cur->next = split;
            }
            if (prev)
                prev->next = cur->next;
            else
                free_list = cur->next;
            cur->next = NULL;
            return (void *)((uint8_t *)cur + sizeof(block_t));
        }
        prev = cur;
        cur = cur->next;
    }

    size_t pages = (size + sizeof(block_t) + 0xFFF) / 0x1000;
    heap_expand(pages);
    return kmalloc(size);
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    block_t *blk = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    blk->next = free_list;
    free_list = blk;
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    block_t *blk = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    if (blk->size >= size)
        return ptr;

    void *new_ptr = kmalloc(size);
    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, blk->size);
    kfree(ptr);
    return new_ptr;
}