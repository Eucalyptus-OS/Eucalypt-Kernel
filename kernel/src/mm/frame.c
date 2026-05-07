#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <mm/hhdm.h>
#include <mm/types.h>
#include <mm/frame.h>

__attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

typedef struct frame_node {
    struct frame_node *next;
} frame_node_t;

frame_node_t *frame_free_list = NULL;

uint64_t frame_alloc() {
    if (frame_free_list == NULL) {
        return 0;
    }

    frame_node_t *node = frame_free_list;
    frame_free_list = node->next;
    return virt_phys((vaddr)node);
}

void frame_free(paddr ptr) {
    frame_node_t *node = (frame_node_t *)phys_virt(ptr);
    node->next = frame_free_list;
    frame_free_list = node;
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