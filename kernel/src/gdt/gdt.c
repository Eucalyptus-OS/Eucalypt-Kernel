#include <stdint.h>
#include <gdt/gdt.h>

extern void reload();

uint8_t gdt[7][8];

struct [[gnu::packed]] gdtr {
    uint16_t limit;
    uint64_t base;
};

tss_t tss = {0};

void encode_gdt_entry(uint8_t index, uint32_t base, uint32_t limit, uint8_t ab, uint8_t flags) {
    if (limit > 0xFFFFF) {
        return;
    }
    uint8_t *target = gdt[index];

    // Limit
    target[0] = limit & 0xFF;
    target[1] = (limit >> 8) & 0xFF;
    target[6] = (limit >> 16) & 0x0F;

    // Base
    target[2] = base & 0xFF;            // Base lo
    target[3] = (base >> 8) & 0xFF;     // Base mid lo
    target[4] = (base >> 16) & 0xFF;    // Base mid hi
    target[7] = (base >> 24) & 0xFF;    // Base hi
    
    // AB
    target[5] = ab;
    // Flags
    target[6] |= (flags << 4);
}

void encode_tss_descriptor(uint8_t index, tss_t *tss_ptr) {
    uint64_t base  = (uint64_t)(uintptr_t)tss_ptr;
    uint32_t limit = sizeof(tss_t) - 1;

    uint8_t *lo = gdt[index];
    uint8_t *hi = gdt[index + 1];

    // Limit
    lo[0] = limit & 0xFF;
    lo[1] = (limit >> 8) & 0xFF;

    // Base
    lo[2] = base & 0xFF;
    lo[3] = (base >> 8) & 0xFF;
    lo[4] = (base >> 16) & 0xFF;

    // Access byte
    lo[5] = 0x89;

    // Flags
    lo[6] = ((limit >> 16) & 0x0F);

    // Base
    lo[7] = (base >> 24) & 0xFF;

    // Base
    hi[0] = (base >> 32) & 0xFF;
    hi[1] = (base >> 40) & 0xFF;
    hi[2] = (base >> 48) & 0xFF;
    hi[3] = (base >> 56) & 0xFF;

    // Bytes
    hi[4] = 0;
    hi[5] = 0;
    hi[6] = 0;
    hi[7] = 0;
}

void gdt_init() {
    struct gdtr gdtr = {
        .limit = sizeof(gdt) - 1,
        .base = (uint64_t)(uintptr_t)gdt,
    };
    encode_gdt_entry(0, 0, 0x00000000, 0x00, 0x0);
    encode_gdt_entry(1, 0, 0xFFFFF, 0x9A, 0xA);
    encode_gdt_entry(2, 0, 0xFFFFF, 0x92, 0xC);
    encode_gdt_entry(3, 0, 0xFFFFF, 0xF2, 0xC);
    encode_gdt_entry(4, 0, 0xFFFFF, 0xFA, 0xA);
    encode_tss_descriptor(5, &tss);

    asm volatile ("lgdt %0" :: "m"(gdtr));
    reload();
    asm volatile ("ltr %0" :: "r"((uint16_t)0x28));
}