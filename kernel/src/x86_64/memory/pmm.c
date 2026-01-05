#include <x86_64/memory/pmm.h>
#include <x86_64/memory/vmm.h>
#include <x86_64/serial.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

extern volatile struct limine_memmap_request memmap_request;

static uint8_t* bitmap;
static uint64_t total_pages, used_pages;

static inline void bitmap_set(uint64_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(uint64_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline bool bitmap_test(uint64_t bit) {
    return bitmap[bit / 8] & (1 << (bit % 8));
}

static uint64_t find_free_pages(size_t count) {
    for (uint64_t i = 0; i < total_pages; i++) {
        bool found = true;
        for (size_t j = 0; j < count && found; j++) {
            if (i + j >= total_pages || bitmap_test(i + j)) {
                found = false;
            }
        }
        if (found) {
            return i;
        }
    }
    return (uint64_t)-1;
}

void pmm_init() {
    struct limine_memmap_response* memmap = memmap_request.response;
    
    uint64_t highest = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t top = entry->base + entry->length;
            if (top > highest) highest = top;
        }
    }
    
    total_pages = highest / PAGE_SIZE;
    uint64_t bitmap_size = (total_pages + 7) / 8;
    
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            bitmap = (uint8_t*)phys_to_virt(entry->base);
            break;
        }
    }
    
    if (!bitmap) {
        serial_print("ERROR: Could not find space for bitmap!\n");
        return;
    }
    
    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
    }
    
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry* entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t base_page = entry->base / PAGE_SIZE;
            uint64_t page_count = entry->length / PAGE_SIZE;
            for (uint64_t j = 0; j < page_count; j++) {
                bitmap_clear(base_page + j);
            }
        }
    }
    
    uint64_t bitmap_start = virt_to_phys(bitmap) / PAGE_SIZE;
    uint64_t bitmap_end = (virt_to_phys(bitmap) + bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    for (uint64_t i = bitmap_start; i < bitmap_end; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
        }
    }
    
    serial_print("PMM initialized: ");
    serial_print_hex(total_pages);
    serial_print(" pages, ");
    serial_print_hex(pmm_get_free_memory() / 1024 / 1024);
    serial_print(" MB free\n");
    
    void *test1 = pmm_alloc();
    void *test2 = pmm_alloc();
    void *test3 = pmm_alloc();
    
    if (test1 && test2 && test3) {
        serial_print("PMM self-test: PASSED\n");
        pmm_free(test1);
        pmm_free(test2);
        pmm_free(test3);
    } else {
        serial_print("PMM self-test: FAILED\n");
    }
}

void* pmm_alloc() {
    uint64_t page = find_free_pages(1);
    if (page == (uint64_t)-1) return NULL;
    
    bitmap_set(page);
    used_pages++;
    return (void*)(page * PAGE_SIZE);
}

void* pmm_alloc_pages(size_t count) {
    uint64_t page = find_free_pages(count);
    if (page == (uint64_t)-1) return NULL;
    
    for (size_t i = 0; i < count; i++) {
        bitmap_set(page + i);
        used_pages++;
    }
    return (void*)(page * PAGE_SIZE);
}

void pmm_free(void* ptr) {
    if (!ptr) return;
    
    uint64_t page = (uint64_t)ptr / PAGE_SIZE;
    if (page >= total_pages || !bitmap_test(page)) return;
    
    bitmap_clear(page);
    used_pages--;
}

void pmm_free_pages(void* ptr, size_t count) {
    if (!ptr) return;
    
    uint64_t page = (uint64_t)ptr / PAGE_SIZE;
    for (size_t i = 0; i < count; i++) {
        if (page + i < total_pages && bitmap_test(page + i)) {
            bitmap_clear(page + i);
            used_pages--;
        }
    }
}

uint64_t pmm_get_total_memory() {
    return total_pages * PAGE_SIZE;
}

uint64_t pmm_get_free_memory() {
    return (total_pages - used_pages) * PAGE_SIZE;
}