#include <mm/paging.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <memory.h>
#include <stdint.h>
#include <limine.h>

#define ENTRIES_PER_TABLE 512
#define PTE_PHYS_MASK 0x000FFFFFFFFFF000ULL

extern char __text_start[];
extern char __text_end[];
extern char __rodata_start[];
extern char __rodata_end[];
extern char __data_start[];
extern char __data_end[];

extern volatile struct limine_framebuffer_request framebuffer_request;

uint64_t *kernel_pml4;

static inline void reload_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(val) : "memory");
}

static int table_is_empty(uint64_t *table) {
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (table[i] & PAGE_PRESENT) {
            return 0;
        }
    }
    return 1;
}

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

uintptr_t create_pml4() {
    uintptr_t pml4_phys = frame_alloc();
    if (!pml4_phys) {
        return 0;
    }
    uint64_t *kernel_virt = phys_to_virt((uintptr_t)kernel_pml4);
    uint64_t *pml4_virt = phys_to_virt(pml4_phys);
    memset(pml4_virt, 0, PAGE_SIZE);
    for (int i = 256; i < 512; i++) 
        pml4_virt[i] = kernel_virt[i];

    return pml4_phys;
}

uint8_t map_page(uint64_t *pml4, void *virt, uintptr_t phys, uint64_t flags) {
    if (!pml4 || !virt) {
        return 1;
    }

    uint64_t i4 = get_level(virt, 4);
    if (!(pml4[i4] & PAGE_PRESENT)) {
        uintptr_t new_phys = frame_alloc();
        if (!new_phys) {
            return 1;
        }
        memset(phys_to_virt(new_phys), 0, PAGE_SIZE);
        pml4[i4] = new_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }
    uint64_t *pml3 = (uint64_t *)phys_to_virt(pml4[i4] & PTE_PHYS_MASK);

    uint64_t i3 = get_level(virt, 3);
    if (!(pml3[i3] & PAGE_PRESENT)) {
        uintptr_t new_phys = frame_alloc();
        if (!new_phys) {
            return 1;
        }
        memset(phys_to_virt(new_phys), 0, PAGE_SIZE);
        pml3[i3] = new_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }
    uint64_t *pml2 = (uint64_t *)phys_to_virt(pml3[i3] & PTE_PHYS_MASK);

    uint64_t i2 = get_level(virt, 2);
    if (!(pml2[i2] & PAGE_PRESENT)) {
        uintptr_t new_phys = frame_alloc();
        if (!new_phys) {
            return 1;
        }
        memset(phys_to_virt(new_phys), 0, PAGE_SIZE);
        pml2[i2] = new_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }
    uint64_t *pml1 = (uint64_t *)phys_to_virt(pml2[i2] & PTE_PHYS_MASK);

    uint64_t i1 = get_level(virt, 1);
    pml1[i1] = (uint64_t)phys | PAGE_PRESENT | flags;
    return 0;
}

void free_page(uint64_t *pml4, void *virt) {
    if (!pml4 || !virt) {
        return;
    }

    uint64_t i4 = get_level(virt, 4);
    if (!(pml4[i4] & PAGE_PRESENT)) {
        return;
    }
    uint64_t *pml3 = (uint64_t *)phys_to_virt(pml4[i4] & PTE_PHYS_MASK);

    uint64_t i3 = get_level(virt, 3);
    if (!(pml3[i3] & PAGE_PRESENT)) {
        return;
    }
    uint64_t *pml2 = (uint64_t *)phys_to_virt(pml3[i3] & PTE_PHYS_MASK);

    uint64_t i2 = get_level(virt, 2);
    if (!(pml2[i2] & PAGE_PRESENT)) {
        return;
    }
    uint64_t *pml1 = (uint64_t *)phys_to_virt(pml2[i2] & PTE_PHYS_MASK);

    uint64_t i1 = get_level(virt, 1);
    if (!(pml1[i1] & PAGE_PRESENT)) {
        return;
    }

    frame_free(pml1[i1] & PTE_PHYS_MASK);
    pml1[i1] = 0;
    invlpg(virt);

    if (table_is_empty(pml1)) {
        frame_free(virt_to_phys(pml1));
        pml2[i2] = 0;

        if (table_is_empty(pml2)) {
            frame_free(virt_to_phys(pml2));
            pml3[i3] = 0;

            if (table_is_empty(pml3)) {
                frame_free(virt_to_phys(pml3));
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

uint8_t paging_init(struct limine_memmap_response *memmap, struct limine_executable_address_response *exec) {
    uintptr_t pml4 = frame_alloc();

    if (!pml4) {
        return 1;
    }

    memset(phys_to_virt(pml4), 0, PAGE_SIZE);

    uint64_t *vml4 = (uint64_t *)phys_to_virt(pml4);

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE 
            || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_ACPI_NVS
            || entry->type == LIMINE_MEMMAP_RESERVED
            || entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            for (uint64_t offset = 0; offset < entry->length; offset += PAGE_SIZE) {
                if (map_page(vml4,
                                     phys_to_virt(entry->base + offset),
                                     entry->base + offset,
                                     PAGE_WRITABLE | PAGE_NXE)) {
                    return 1;
                }
            }
        }
    }

    if (map_framebuffer(vml4)) {
        return 1;
    }

    if (map_kernel_range(vml4,
                          (uintptr_t)__text_start, (uintptr_t)__text_end,
                          exec->physical_base, exec->virtual_base,
                          0)) {
        return 1;
    }

    if (map_kernel_range(vml4,
                          (uintptr_t)__rodata_start, (uintptr_t)__rodata_end,
                          exec->physical_base, exec->virtual_base,
                          PAGE_NXE)) {
        return 1;
    }

    if (map_kernel_range(vml4,
                          (uintptr_t)__data_start, (uintptr_t)__data_end,
                          exec->physical_base, exec->virtual_base,
                          PAGE_WRITABLE | PAGE_NXE)) {
        return 1;
    }
    
    if (map_kernel_range(vml4,
                      exec->virtual_base, (uintptr_t)__text_start,
                      exec->physical_base, exec->virtual_base,
                      PAGE_NXE)) {
        return 1;
    }

    reload_cr3((uint64_t)pml4);

    kernel_pml4 = (uint64_t *)pml4;

    return 0;
}