#include <stdint.h>
#include <limine.h>
#include <mm/hhdm.h>

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

uint64_t page_list = 0;

uint64_t frame_alloc() {
    uint64_t page = page_list;

    if (page == 0) {
        return 0;
    }

    uint64_t *next_ptr = (uint64_t *)phys_virt(page);
    page_list = *next_ptr;
    return page;
}

void frame_free(uint64_t ptr) {
    uint64_t *next_ptr = (uint64_t *)phys_virt(ptr);

    *next_ptr = page_list;

    page_list = ptr;
}

void frame_init() {
    struct limine_memmap_response *memmap = memmap_request.response;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        for (uint64_t j = entry->base; j < entry->base + entry->length; j += 4096) {
            frame_free(j);
        }
    }
}