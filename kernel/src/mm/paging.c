
#include <mm/frame.h>
#include <memory.h>
#include <stdint.h>

#define PAGE_SIZE 0x1000
#define PAGE_PRESENT 0x1
#define PAGE_WRITABLE (0x1 << 1)
#define PAGE_USER (0x1 << 2)
#define PAGE_WRITE_THROUGH (0x1 << 3)
#define PAGE_DISABLE_CACHE (0x1 << 4)
#define PAGE_ACCESSED (0x1 << 5)
#define PAGE_DIRTY (0x1 << 6)
#define PAGE_NXE (1ULL << 63)
#define ENTRIES_PER_TABLE 512

extern volatile struct limine_framebuffer_request framebuffer_request;

uint64_t *kernel_pml4;

static inline void invlpg(void *addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

uint64_t get_level(void *v, uint8_t level) {
    switch (level) {
    case 4:
        return ((uint64_t)v >> 39) & 0x1FF;
    case 3:
        return ((uint64_t)v >> 30) & 0x1FF;
    case 2:
        return ((uint64_t)v >> 21) & 0x1FF;
    case 1:
        return ((uint64_t)v >> 12) & 0x1FF;
    }
    return 0;
}

void map_page(uint64_t *pml4, void *virt, uintptr_t phys, uint64_t flags) {
    if (!pml4 || !virt) {
        return;
    }

    uint64_t i4 = get_level(virt, 4);
    if (!(pml4[i4] & PAGE_PRESENT)) {
        uint64_t *new_table = (uint64_t *)frame_alloc();
        if (!new_table) {
            return;
        }
        memset(new_table, 0, PAGE_SIZE);
        pml4[i4] = (uint64_t)new_table | PAGE_PRESENT | flags;
    }
    uint64_t *pml3 = (uint64_t *)(pml4[i4] & ~0xFFFULL);

    uint64_t i3 = get_level(virt, 3);
    if (!(pml3[i3] & PAGE_PRESENT)) {
        uint64_t *new_table = (uint64_t *)frame_alloc();
        if (!new_table) {
            return;
        }
        memset(new_table, 0, PAGE_SIZE);
        pml3[i3] = (uint64_t)new_table | PAGE_PRESENT | flags;
    }
    uint64_t *pml2 = (uint64_t *)(pml3[i3] & ~0xFFFULL);

    uint64_t i2 = get_level(virt, 2);
    if (!(pml2[i2] & PAGE_PRESENT)) {
        uint64_t *new_table = (uint64_t *)frame_alloc();
        if (!new_table) {
            return;
        }
        memset(new_table, 0, PAGE_SIZE);
        pml2[i2] = (uint64_t)new_table | PAGE_PRESENT | flags;
    }
    uint64_t *pml1 = (uint64_t *)(pml2[i2] & ~0xFFFULL);

    uint64_t i1 = get_level(virt, 1);
    pml1[i1] = (uint64_t)phys | PAGE_PRESENT | flags;
}

static int table_is_empty(uint64_t *table) {
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (table[i] & PAGE_PRESENT) {
            return 0;
        }
    }
    return 1;
}

void free_page(uint64_t *pml4, void *virt) {
    if (!pml4 || !virt) {
        return;
    }

    uint64_t i4 = get_level(virt, 4);
    if (!(pml4[i4] & PAGE_PRESENT)) {
        return;
    }
    uint64_t *pml3 = (uint64_t *)(pml4[i4] & ~0xFFFULL);

    uint64_t i3 = get_level(virt, 3);
    if (!(pml3[i3] & PAGE_PRESENT)) {
        return;
    }
    uint64_t *pml2 = (uint64_t *)(pml3[i3] & ~0xFFFULL);

    uint64_t i2 = get_level(virt, 2);
    if (!(pml2[i2] & PAGE_PRESENT)) {
        return;
    }
    uint64_t *pml1 = (uint64_t *)(pml2[i2] & ~0xFFFULL);

    uint64_t i1 = get_level(virt, 1);
    if (!(pml1[i1] & PAGE_PRESENT)) {
        return;
    }

    frame_free(pml1[i1] & ~0xFFFULL);
    pml1[i1] = 0;
    invlpg(virt);

    if (table_is_empty(pml1)) {
        frame_free((uintptr_t)pml1);
        pml2[i2] = 0;

        if (table_is_empty(pml2)) {
            frame_free((uintptr_t)pml2);
            pml3[i3] = 0;

            if (table_is_empty(pml3)) {
                frame_free((uintptr_t)pml3);
                pml4[i4] = 0;
            }
        }
    }
}


static uint8_t map_kernel_range(uint64_t *pml4, uintptr_t virt_start, uintptr_t virt_end,
                                 uintptr_t phys_base, uintptr_t virt_base, uint64_t flags) {
    uintptr_t start = virt_start & ~0xFFFULL;
    uintptr_t end = (virt_end + PAGE_SIZE - 1) & ~0xFFFULL;

    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        uintptr_t pa = phys_base + (va - virt_base);
        if (map_page(pml4, (void *)va, pa, flags)) {
            return 1;
        }
    }

    return 0;
}

static uint8_t map_framebuffer(uint64_t *pml4) {
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        return 0;
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    uintptr_t fb_phys_start = virt_to_phys(fb->address);
    uintptr_t fb_virt_start = (uintptr_t)phys_to_virt(fb_phys_start) & ~0xFFFULL; 
    size_t fb_size = (size_t)fb->pitch * (size_t)fb->height;
    size_t fb_pages_size = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    print("FRAMEBUFFER limine_addr=%lx phys=%lx virt=%lx size=%lx pitch=%u height=%u\n",
          (unsigned long)fb->address, (unsigned long)fb_phys_start, (unsigned long)fb_virt_start,
          (unsigned long)fb_pages_size, (unsigned)fb->pitch, (unsigned)fb->height);

    for (size_t off = 0; off < fb_pages_size; off += PAGE_SIZE) {
        if (map_page(pml4,
                            (void *)(fb_virt_start + off),
                            fb_phys_start + off,
                            PAGE_WRITABLE | PAGE_NXE)) {
            return 1;
        }
    }

    fb->address = (void*)fb_virt_start; 

    return 0;
}


uint8_t paging_prepare_kernel_stack_region() {
    if (!kernel_pml4) {
        return 1;
    }

    uint64_t *pml4 = phys_to_virt((uintptr_t)kernel_pml4);
    uint64_t idx = get_pml(4, (void *)KERNEL_STACK_REGION);

    if (pml4[idx] & PAGE_PRESENT) {
        return 0;
    }

    uintptr_t table = frame_alloc();
    if (!table) {
        return 1;
    }

    memset(phys_to_virt(table), 0, PAGE_SIZE);
    pml4[idx] = table | PAGE_PRESENT | PAGE_WRITABLE;

    return 0;
}

uint8_t paging_init(struct limine_memmap_response *memmap, struct limine_executable_address_response *exec) {
    uintptr_t pml4 = frame_alloc();

    if (!pml4) {
        return 1;
    }

    memset(phys_to_virt(pml4), 0, PAGE_SIZE);

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE 
            || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_ACPI_NVS
            || entry->type == LIMINE_MEMMAP_RESERVED
            || entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            for (uint64_t offset = 0; offset < entry->length; offset += PAGE_SIZE) {
                if (map_page((uint64_t *)pml4,
                                     phys_to_virt(entry->base + offset),
                                     entry->base + offset,
                                     PAGE_WRITABLE | PAGE_NXE)) {
                    return 1;
                }
            }
        }
    }

    if (map_framebuffer((uint64_t *)pml4)) {
        return 1;
    }

    if (map_kernel_range((uint64_t *)pml4,
                          (uintptr_t)__text_start, (uintptr_t)__text_end,
                          exec->physical_base, exec->virtual_base,
                          0)) {
        return 1;
    }

    if (map_kernel_range((uint64_t *)pml4,
                          (uintptr_t)__rodata_start, (uintptr_t)__rodata_end,
                          exec->physical_base, exec->virtual_base,
                          PAGE_NXE)) {
        return 1;
    }

    if (map_kernel_range((uint64_t *)pml4,
                          (uintptr_t)__data_start, (uintptr_t)__data_end,
                          exec->physical_base, exec->virtual_base,
                          PAGE_WRITABLE | PAGE_NXE)) {
        return 1;
    }
    
    if (map_kernel_range((uint64_t *)pml4,
                      exec->virtual_base, (uintptr_t)__text_start,
                      exec->physical_base, exec->virtual_base,
                      PAGE_NXE)) {
        return 1;
    }

    reload_cr3((uint64_t)pml4);

    kernel_pml4 = (uint64_t *)pml4;

    if (paging_prepare_kernel_stack_region()) {
        return 1;
    }

    return 0;
}