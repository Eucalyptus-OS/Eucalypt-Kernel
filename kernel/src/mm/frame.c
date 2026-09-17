#include <stdint.h>
#include <limine.h>
#include <lib/errno.h>
#include <mm/memory.h>
#include <mm/hhdm.h>
#include <mm/frame.h>

#define PAGE_SIZE 0x1000

static uint8_t *bitmap;
static uintptr_t phys_base;
static uint64_t total_pages;
static uintptr_t bitmap_phys;

static inline uint8_t test_bit(uint64_t bit) {
    return (uint8_t)((bitmap[bit >> 3] >> (bit & 7)) & 1);
}

static inline void set_bit(uint64_t bit) {
    bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static inline void clear_bit(uint64_t bit) {
    bitmap[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

uintptr_t frame_alloc() {
    if (!bitmap) {
        return 0;
    }

    for (uint64_t i = 0; i < total_pages; i++) {
        if (test_bit(i)) {
            clear_bit(i);
            return phys_base + i * PAGE_SIZE;
        }
    }
    return 0;
}

uintptr_t frame_alloc_contig(uint64_t count) {
    if (!bitmap || count == 0 || count > total_pages) {
        return 0;
    }

    uint64_t run = 0;
    for (uint64_t i = 0; i <= total_pages; i++) {
        if (i < total_pages && test_bit(i)) {
            run++;
        } else {
            if (run >= count) {
                uint64_t start = i - run;
                for (uint64_t j = 0; j < count; j++) {
                    clear_bit(start + j);
                }
                return phys_base + start * PAGE_SIZE;
            }
            run = 0;
        }
    }
    return 0;
}

void frame_free(uintptr_t ptr) {
    if (!ptr) {
        return;
    }
    if (ptr < phys_base || ptr >= phys_base + total_pages * PAGE_SIZE) {
        return;
    }

    set_bit((ptr - phys_base) / PAGE_SIZE);
}

void frame_init(struct limine_memmap_response *memmap) {
    bitmap = NULL;
    phys_base = 0;
    total_pages = 0;
    bitmap_phys = 0;

    uintptr_t min_base = 0;
    uintptr_t max_end = 0;
    uintptr_t top = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        if (min_base == 0 || entry->base < min_base) min_base = entry->base;
        if (entry->base + entry->length > max_end) max_end = entry->base + entry->length;
        uintptr_t aligned = (entry->base + entry->length) & ~(PAGE_SIZE - 1);
        if (aligned > top) top = aligned;
    }

    if (max_end == 0 || top == 0) {
        return;
    }

    phys_base = min_base & ~(PAGE_SIZE - 1);
    total_pages = (max_end - phys_base + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t bmp_bytes = (total_pages + 7) / 8;
    uint64_t bmp_pages = (bmp_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    bitmap_phys = top - bmp_pages * PAGE_SIZE;
    bitmap = (uint8_t *)phys_to_virt(bitmap_phys);

    memset(bitmap, 0x00, bmp_pages * PAGE_SIZE);

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        uintptr_t s = entry->base;
        if (s < phys_base) s = phys_base;
        uintptr_t e = entry->base + entry->length;
        if (e > phys_base + total_pages * PAGE_SIZE) e = phys_base + total_pages * PAGE_SIZE;

        for (uintptr_t pa = s; pa < e; pa += PAGE_SIZE) {
            if (pa >= bitmap_phys && pa < bitmap_phys + bmp_pages * PAGE_SIZE) {
                continue;
            }
            set_bit((pa - phys_base) / PAGE_SIZE);
        }
    }
}
