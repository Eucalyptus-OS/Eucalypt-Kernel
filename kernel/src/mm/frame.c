#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <mm/frame.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

struct page_node {
    struct page_node *next;
};

struct page_node *page_list;

uint64_t frame_alloc() {
    struct page_node *node = page_list;
    if (!node) return 0;
    page_list = node->next;
    return (uint64_t)node;
}

void frame_free(uint64_t addr) {
    if (!addr) return;
    struct page_node *node = (struct page_node *)addr;
    node->next = page_list;
    page_list = node;
}

uint8_t frame_init() {
    if (memmap_request.response == NULL) {
        return 1;
    }
    struct limine_memmap_response response = *memmap_request.response;
    uint64_t entry_count = response.entry_count;
    struct limine_memmap_entry **entries = response.entries;

    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i]->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        for (uint64_t j = entries[i]->base;
             j < entries[i]->base + entries[i]->length;
             j += 0x1000) {
            frame_free(j);
        }
    }
    return 0;
}