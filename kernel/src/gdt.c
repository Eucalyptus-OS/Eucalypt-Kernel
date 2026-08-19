#include <stdint.h>
#include <gdt.h>

extern void reload();

uint8_t gdt_table[8][8] = {0};

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__ ((packed));

// Global for the CPU
static struct tss global_tss = {0};

static inline void set_descriptor(uint8_t index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    uint8_t *target = gdt_table[index];

    target[0] = limit & 0xFF;
    target[1] = (limit >> 8) & 0xFF;
    target[6] = (limit >> 16) & 0x0F;
    target[2] = base & 0xFF;
    target[3] = (base >> 8) & 0xFF;
    target[4] = (base >> 16) & 0xFF;
    target[7] = (base >> 24) & 0xFF;
    target[5] = access;
    target[6] |= (flags << 4);
}

static inline void set_tss_descriptor(uint8_t index, uint64_t base, uint32_t limit, uint8_t access) {
    set_descriptor(index, (uint32_t)(base & 0xFFFFFFFF), limit, access, 0x0);

    uint8_t *target = gdt_table[index + 1];

    uint32_t base_hi = (uint32_t)(base >> 32); // The upper 32 bits of the TSS base
    target[0] = base_hi & 0xFF;
    target[1] = (base_hi >> 8) & 0xFF;
    target[2] = (base_hi >> 16) & 0xFF;
    target[3] = (base_hi >> 24) & 0xFF;

    target[4] = 0;
    target[5] = 0;
    target[6] = 0;
    target[7] = 0;
}

static inline void write_tss() {
    uint64_t base = (uint64_t)&global_tss;
    uint32_t limit = sizeof(struct tss) - 1;

    global_tss.iomap_base = sizeof(struct tss);
    set_tss_descriptor(5, base, limit, 0x89); // TSS Descriptor
}

void gdt_init() {
    set_descriptor(0, 0, 0, 0, 0);
    set_descriptor(1, 0, 0xFFFFFFFF, 0x9A, 0xA);
    set_descriptor(2, 0, 0xFFFFFFFF, 0x92, 0xC);
    set_descriptor(3, 0, 0xFFFFFFFF, 0xF2, 0xC);
    set_descriptor(4, 0, 0xFFFFFFFF, 0xFA, 0xA);

    write_tss();
    struct gdtr ptr = {
        .limit = sizeof(gdt_table) - 1,
        .base = (uint64_t)&gdt_table
    };

    asm volatile ("lgdt %0" :: "m"(ptr));

    uint16_t tss_selector = 0x28;
    asm volatile ("ltr %0" :: "r"(tss_selector));
    reload();
}