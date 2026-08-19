#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <mm/hhdm.h>
#include <mm/frame.h>

uint64_t frame_list;

uintptr_t frame_alloc() {
    if (frame_list == 0) return 0;
    uintptr_t frame = frame_list;
    frame_list = *(uint64_t *)phys_to_virt(frame);
    return frame;
}

void frame_free(uintptr_t frame) {
    uint64_t *frame_ptr = (uint64_t *)phys_to_virt(frame);
    *frame_ptr = frame_list;
    frame_list = frame;
}

uint8_t frame_init(struct limine_memmap_response *memmap) {
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uintptr_t start = entry->base;
            uintptr_t end = entry->base + entry->length;
            for (uint64_t frame = start; frame < end; frame += 0x1000) {
                frame_free((uintptr_t)frame);
            }
        }
    }
    return 0;
}