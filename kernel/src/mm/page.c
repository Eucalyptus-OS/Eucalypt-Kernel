#include <stdint.h>
#include <limine.h>
#include <mm/memory.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <logging/print.h>
#include <mm/page.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

#define PAGE_SIZE 0x1000

#define PAGE_PRESENT 0x1
#define PAGE_WRITABLE (0x1 << 1)
#define PAGE_USER (0x1 << 2)
#define PAGE_WRITE_THROUGH (0x1 << 3)
#define PAGE_DISABLE_CACHE (0x1 << 4)
#define PAGE_ACCESSED (0x1 << 5)
#define PAGE_DIRTY (0x1 << 6)
#define PAGE_NXE (1ULL << 63)

#define PAT0 (0)
#define PAT1 (ENTRY_FLAG_WRITETHROUGH)
#define PAT2 (ENTRY_FLAG_DISABLECACHE)
#define PAT3 (ENTRY_FLAG_DISABLECACHE | ENTRY_FLAG_WRITETHROUGH)
#define PAT4(PAT_FLAG) (PAT_FLAG)
#define PAT5(PAT_FLAG) ((PAT_FLAG) | ENTRY_FLAG_WRITETHROUGH)
#define PAT6(PAT_FLAG) ((PAT_FLAG) | ENTRY_FLAG_DISABLECACHE)
#define PAT7(PAT_FLAG) ((PAT_FLAG) | ENTRY_FLAG_DISABLECACHE | ENTRY_FLAG_WRITETHROUGH)

#define GET_PAGE_OFFSET(addr) ((addr) & 0xFFF)

#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000ULL // masks off the flag bits to isolate the physical frame address

uint64_t *kernel_pml4 = NULL; // physical address of the kernel's root PML4, shared by all address spaces

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];

typedef uint64_t page_entry_t; // 64-bit entry: [51:12] frame address, [11:0] flags, [63] NX

// Read the current PML4 physical address out of CR3
static inline uintptr_t get_current_cr3() {
    uintptr_t cr3 = 0;
    asm volatile ("mov %%cr3, %0" : "=r"(cr3) :: "memory");
    return cr3;
}

// TLB-flush every 4 KiB page in [addr, addr+len) using invlpg
static void invalidate_page(void *addr, uint64_t len) {
    for (uint64_t i = 0; i < len; i += 0x1000) {
        asm volatile("invlpg (%0)" : : "r"(addr + i) : "memory");
    }
}

// Switch address spaces by loading a new PML4 physical address into CR3
void reload_cr3(uint64_t pml_to_load) {
    asm volatile("mov %0, %%cr3" : : "r"(pml_to_load) : "memory");
}

// Extract the table index for a paging level (1=PT, 2=PD, 3=PDPT, 4=PML4) from a virtual address
uint64_t get_pml(uint8_t level, void *addr) {
    if (level > 4 || level < 1) {
        return 0;
    }

    if (!addr) {
        return 0;
    }

    switch (level){
        case 1: 
            return ((uint64_t)addr >> 12) & 0x1FF; 
        case 2:
            return ((uint64_t)addr >> 21) & 0x1FF;
        case 3:
            return ((uint64_t)addr >> 30) & 0x1FF;
        case 4:
            return ((uint64_t)addr >> 39) & 0x1FF;
        default:
            break;
    }

    return 0;
}

// Allocate a fresh PML4 that shares the kernel's upper-half entries with the current tables
uintptr_t paging_create_pml4() {
    uintptr_t pml4_phys = frame_alloc();
    uint64_t *l_kernel = phys_to_virt((uintptr_t)kernel_pml4);

    if (!pml4_phys) {
        return 0;
    }

    uint64_t *pml4_virt = phys_to_virt(pml4_phys);

    memset(pml4_virt, 0, PAGE_SIZE);

    for (int i = 256; i < 512; i++) { // i >= 256 is the higher half, identical across all address spaces
        pml4_virt[i] = l_kernel[i];
    }

    return pml4_phys;
}

// Deep-copy src_table into dst_table, cloning every present (non-huge-page) child table
static uint8_t clone_address_space_table(uint64_t *src_table, uint64_t *dst_table, uint64_t level) {
    for (int i = 0; i < 512; i++) {
        uint64_t entry = src_table[i];
        if (!(entry & PAGE_PRESENT)) {
            continue;
        }

        if (level == 1) { // leaf: copy the frame contents so user pages become independent copies
            uintptr_t new_frame = frame_alloc();
            if (!new_frame) {
                return 1;
            }

            uintptr_t src_frame = entry & PAGE_ADDR_MASK;
            memcpy(phys_to_virt(new_frame), phys_to_virt(src_frame), PAGE_SIZE);
            dst_table[i] = (new_frame & PAGE_ADDR_MASK) | (entry & ~PAGE_ADDR_MASK);
        } else {
            if (entry & (1ULL << 7)) { // bit 7 (PS) marks a huge page; only 4 KiB entries are cloned
                return 1;
            }

            uintptr_t new_table = frame_alloc();
            if (!new_table) {
                return 1;
            }

            uint64_t *dst_subtable = phys_to_virt(new_table);
            memset(dst_subtable, 0, PAGE_SIZE);
            dst_table[i] = (new_table & PAGE_ADDR_MASK) | (entry & ~PAGE_ADDR_MASK);

            uint64_t *src_subtable = phys_to_virt(entry & PAGE_ADDR_MASK);
            if (clone_address_space_table(src_subtable, dst_subtable, level - 1)) {
                return 1;
            }
        }
    }

    return 0;
}

// Deep-copy the current user address space into a fresh PML4 (for fork)
uintptr_t fork_address_space() {
    uintptr_t new_pml4_phys = frame_alloc();
    if (!new_pml4_phys) {
        return 0;
    }

    uint64_t *src_pml4 = phys_to_virt(get_current_cr3());
    uint64_t *dst_pml4 = phys_to_virt(new_pml4_phys);
    uint64_t *kernel = phys_to_virt((uintptr_t)kernel_pml4);

    memset(dst_pml4, 0, PAGE_SIZE);

    for (int i = 256; i < 512; i++) { // kernel half points at the shared kernel_pml4 entries
        dst_pml4[i] = kernel[i];
    }

    for (int i = 0; i < 256; i++) {
        if (!(src_pml4[i] & PAGE_PRESENT)) {
            continue;
        }

        if (src_pml4[i] & (1ULL << 7)) { // huge pages are not supported across forks
            frame_free(new_pml4_phys);
            return 0;
        }

        uintptr_t new_table = frame_alloc();
        if (!new_table) {
            frame_free(new_pml4_phys);
            return 0;
        }

        uint64_t *dst_subtable = phys_to_virt(new_table);
        memset(dst_subtable, 0, PAGE_SIZE);
        dst_pml4[i] = (new_table & PAGE_ADDR_MASK) | (src_pml4[i] & ~PAGE_ADDR_MASK);

        uint64_t *src_subtable = phys_to_virt(src_pml4[i] & PAGE_ADDR_MASK);
        if (clone_address_space_table(src_subtable, dst_subtable, 3)) {
            frame_free(new_pml4_phys);
            return 0;
        }
    }

    return new_pml4_phys;
}

// Recursively free every leaf frame and child table reachable from `table`
static void destroy_table(uint64_t *table, uint64_t level) {
    for (int i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        if (!(entry & PAGE_PRESENT)) {
            continue;
        }

        uintptr_t frame = entry & PAGE_ADDR_MASK;

        if (level == 1) {
            frame_free(frame);
        } else if (!(entry & (1ULL << 7))) { // skip huge-page leaves: no child table to descend into
            destroy_table(phys_to_virt(frame), level - 1);
            frame_free(frame);
        }
    }
}

// Tear down a user address space, leaving the shared kernel half untouched
void paging_destroy_address_space(uintptr_t pml4_phys) {
    if (!pml4_phys) {
        return;
    }

    uint64_t *pml4 = phys_to_virt(pml4_phys);

    for (int i = 0; i < 256; i++) {
        uint64_t entry = pml4[i];
        if (!(entry & PAGE_PRESENT)) {
            continue;
        }

        uintptr_t frame = entry & PAGE_ADDR_MASK;

        if (!(entry & (1ULL << 7))) { // only descend into non-huge-page (table) entries
            destroy_table(phys_to_virt(frame), 3);
            frame_free(frame);
        }
    }

    frame_free(pml4_phys);
}

// Map one 4 KiB virtual page to phys_addr in the given address space, creating missing tables along the way
uint8_t paging_map_page(uint64_t *pml4_phys, void *virt_addr, uintptr_t phys_addr, uint64_t flags) {
    if (!pml4_phys) {
        return 1;
    }

    uint64_t *pml4 = (uint64_t *)phys_to_virt((uintptr_t)pml4_phys);
    uint64_t pml4_index = get_pml(4, virt_addr);
    // No PDPT entry yet: allocate a zeroed table and link it in
    if (!(pml4[pml4_index] & PAGE_PRESENT)) {
        uintptr_t new_table = frame_alloc();
        if (!new_table) {
            return 1;
        }
        memset(phys_to_virt(new_table), 0, PAGE_SIZE);
        pml4[pml4_index] = new_table | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    uint64_t *pml3 = (uint64_t *)phys_to_virt(pml4[pml4_index] & PAGE_ADDR_MASK);
    uint64_t pml3_index = get_pml(3, virt_addr);
    // No PD entry yet: allocate a zeroed table and link it in
    if (!(pml3[pml3_index] & PAGE_PRESENT)) {
        uintptr_t new_table = frame_alloc();
        if (!new_table) {
            return 1;
        }
        memset(phys_to_virt(new_table), 0, PAGE_SIZE);
        pml3[pml3_index] = new_table | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    uint64_t *pml2 = (uint64_t *)phys_to_virt(pml3[pml3_index] & PAGE_ADDR_MASK);
    uint64_t pml2_index = get_pml(2, virt_addr);
    // No PT entry yet: allocate a zeroed table and link it in
    if (!(pml2[pml2_index] & PAGE_PRESENT)) {
        uintptr_t new_table = frame_alloc();
        if (!new_table) {
            return 1;
        }
        memset(phys_to_virt(new_table), 0, PAGE_SIZE);
        pml2[pml2_index] = new_table | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    uint64_t *pml1 = (uint64_t *)phys_to_virt(pml2[pml2_index] & PAGE_ADDR_MASK);
    uint64_t pml1_index = get_pml(1, virt_addr);
    pml1[pml1_index] = (phys_addr & PAGE_ADDR_MASK) | flags | PAGE_PRESENT; // leaf entry: frame address + flags + present

    invalidate_page(virt_addr, PAGE_SIZE);

    return 0;
}

// Unmap one virtual page and hand its physical frame back to the frame allocator
void paging_unmap_page(uint64_t *pml4_phys, void *virt_addr) {
    if (!pml4_phys) {
        return;
    }

    uint64_t *pml4 = (uint64_t *)phys_to_virt((uintptr_t)pml4_phys);
    uint64_t i4 = get_pml(4, virt_addr);
    if (!(pml4[i4] & PAGE_PRESENT)) return;

    uint64_t *pml3 = (uint64_t *)phys_to_virt(pml4[i4] & PAGE_ADDR_MASK);
    uint64_t i3 = get_pml(3, virt_addr);
    if (!(pml3[i3] & PAGE_PRESENT)) return;

    uint64_t *pml2 = (uint64_t *)phys_to_virt(pml3[i3] & PAGE_ADDR_MASK);
    uint64_t i2 = get_pml(2, virt_addr);
    if (!(pml2[i2] & PAGE_PRESENT)) return;

    uint64_t *pml1 = (uint64_t *)phys_to_virt(pml2[i2] & PAGE_ADDR_MASK);
    uint64_t i1 = get_pml(1, virt_addr);
    if (!(pml1[i1] & PAGE_PRESENT)) return;

    uintptr_t phys_frame = pml1[i1] & PAGE_ADDR_MASK;

    pml1[i1] = 0;
    invalidate_page(virt_addr, PAGE_SIZE);

    frame_free(phys_frame);
}

// Map a kernel image section at its correct physical load address for the given PML4
static uint8_t map_kernel_range(uint64_t *pml4, uintptr_t virt_start, uintptr_t virt_end,
                                 uintptr_t phys_base, uintptr_t virt_base, uint64_t flags) {
    uintptr_t start = virt_start & ~0xFFFULL;
    uintptr_t end = (virt_end + PAGE_SIZE - 1) & ~0xFFFULL;

    for (uintptr_t va = start; va < end; va += PAGE_SIZE) {
        uintptr_t pa = phys_base + (va - virt_base);
        if (paging_map_page(pml4, (void *)va, pa, flags)) {
            return 1;
        }
    }

    return 0;
}

// Map the primary limine framebuffer into the higher half and repoint fb->address at the virtual copy
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
        if (paging_map_page(pml4,
                             (void *)(fb_virt_start + off),
                             fb_phys_start + off,
                             PAGE_WRITABLE | PAGE_NXE)) {
            return 1;
        }
    }

    fb->address = (void*)fb_virt_start; 

    return 0;
}


// Ensure the fixed kernel-stack region has an empty top-level entry so it can be populated later
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

// Build the initial kernel address space: HHDM, kernel sections, framebuffer, and stack region
uint8_t paging_init(struct limine_memmap_response *memmap, struct limine_executable_address_response *exec) {
    uintptr_t pml4 = frame_alloc();

    if (!pml4) {
        return 1;
    }

    memset(phys_to_virt(pml4), 0, PAGE_SIZE);

    // Map every reported physical region into the higher half so phys_to_virt covers all of RAM
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE 
            || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_ACPI_NVS
            || entry->type == LIMINE_MEMMAP_RESERVED
            || entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES
            || entry->type == LIMINE_MEMMAP_RESERVED_MAPPED) {
            for (uint64_t offset = 0; offset < entry->length; offset += PAGE_SIZE) {
                if (paging_map_page((uint64_t *)pml4,
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