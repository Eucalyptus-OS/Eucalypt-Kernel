#include <stddef.h>
#include <stdint.h>
#include <mem.h>
#include <logging/printk.h>
#include <mm/hhdm.h>
#include <mm/frame.h>
#include <mm/paging.h>
#include <mm/heap.h>

#define HEAP_START 0xFFFF900000000000ULL
#define HEAP_END   0xFFFF980000000000ULL
#define HEAP_MAGIC 0xDEADBEEFCAFEBABEULL
#define ALIGN(x)   (((x) + 15) & ~(size_t)15)

typedef struct block {
    uint64_t     magic;
    size_t       size;
    bool         free;
    struct block *next;
    struct block *prev;
} block_t;

static block_t  *free_list = NULL;
static uint64_t  heap_top  = HEAP_START;

static block_t *expand(size_t min_size) {
    size_t pages = (min_size + sizeof(block_t) + 0xFFF) / 0x1000;
    size_t bytes = pages * 0x1000;

    if (heap_top + bytes > HEAP_END) {
        log_info("kmalloc: out of heap space\n");
        return NULL;
    }

    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = frame_alloc();
        if (!phys) {
            log_info("kmalloc: frame_alloc failed\n");
            return NULL;
        }
        paging_map_page(kernel_pml4, heap_top + i * 0x1000, phys, 0x1000,
                        ENTRY_FLAG_PRESENT | ENTRY_FLAG_RW);
    }

    block_t *blk = (block_t *)heap_top;
    blk->magic   = HEAP_MAGIC;
    blk->size    = bytes - sizeof(block_t);
    blk->free    = true;
    blk->next    = NULL;
    blk->prev    = NULL;

    heap_top += bytes;

    // coalesce with previous block if it's free
    if (free_list) {
        block_t *last = free_list;
        while (last->next) last = last->next;
        if (last->free &&
            (uint8_t *)last + sizeof(block_t) + last->size == (uint8_t *)blk) {
            last->size += sizeof(block_t) + blk->size;
            return last;
        }
        last->next = blk;
        blk->prev  = last;
    } else {
        free_list = blk;
    }

    return blk;
}

void heap_init(void) {
    expand(0x1000);
}

void *kmalloc(size_t size) {
    if (!size) return NULL;
    size = ALIGN(size);

    block_t *cur = free_list;
    while (cur) {
        if (cur->free && cur->size >= size) {
            // split if remainder is large enough
            if (cur->size >= size + sizeof(block_t) + 16) {
                block_t *split = (block_t *)((uint8_t *)cur + sizeof(block_t) + size);
                split->magic   = HEAP_MAGIC;
                split->size    = cur->size - size - sizeof(block_t);
                split->free    = true;
                split->next    = cur->next;
                split->prev    = cur;
                if (cur->next) cur->next->prev = split;
                cur->next = split;
                cur->size = size;
            }
            cur->free = false;
            return (void *)((uint8_t *)cur + sizeof(block_t));
        }
        cur = cur->next;
    }

    block_t *blk = expand(size);
    if (!blk) return NULL;
    return kmalloc(size);
}

void kfree(void *ptr) {
    if (!ptr) return;

    block_t *blk = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    if (blk->magic != HEAP_MAGIC) {
        log_info("kfree: bad magic at %llx\n", (uint64_t)ptr);
        return;
    }

    blk->free = true;

    // coalesce next
    if (blk->next && blk->next->free) {
        blk->size += sizeof(block_t) + blk->next->size;
        blk->next  = blk->next->next;
        if (blk->next) blk->next->prev = blk;
    }

    // coalesce prev
    if (blk->prev && blk->prev->free) {
        blk->prev->size += sizeof(block_t) + blk->size;
        blk->prev->next  = blk->next;
        if (blk->next) blk->next->prev = blk->prev;
    }
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr)   return kmalloc(size);
    if (!size)  { kfree(ptr); return NULL; }

    block_t *blk = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    size = ALIGN(size);

    if (blk->size >= size) return ptr;

    void *new = kmalloc(size);
    if (!new) return NULL;
    memcpy(new, ptr, blk->size);
    kfree(ptr);
    return new;
}