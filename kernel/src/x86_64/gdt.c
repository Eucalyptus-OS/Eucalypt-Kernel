#include <x86_64/gdt.h>

#include <stdint.h>
#include <string.h>

#define NULL_DESCRIPTOR 0, 0, 0x00000000, 0x00, 0x0
#define KERNEL_CODE_SEG 1, 0, 0xFFFFF, 0x9A, 0xAF
#define KERNEL_DATA_SEG 2, 0, 0xFFFFF, 0x92, 0xCF
#define USER_DATA_SEG   3, 0, 0xFFFFF, 0xF2, 0xCF
#define USER_CODE_SEG   4, 0, 0xFFFFF, 0xFA, 0xAF

#define GDT_ENTRIES 7

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

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
    uint16_t iopb_offset;
} __attribute__((packed));

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_pointer;
static struct tss kernel_tss;
static uint8_t kernel_stack[4096] __attribute__((aligned(16)));

static void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt[index].base_low = (base & 0xFFFF);
    gdt[index].base_middle = (base >> 16) & 0xFF;
    gdt[index].base_high = (base >> 24) & 0xFF;
    gdt[index].limit_low = (limit & 0xFFFF);
    gdt[index].granularity = (limit >> 16) & 0x0F;
    gdt[index].granularity |= granularity & 0xF0;
    gdt[index].access = access;
}

static void gdt_set_tss(int index, uint64_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    uint64_t desc_low = 0;
    desc_low |= (uint64_t)(limit & 0xFFFF);
    desc_low |= (uint64_t)(base & 0xFFFF) << 16;
    desc_low |= (uint64_t)((base >> 16) & 0xFF) << 32;
    desc_low |= (uint64_t)access << 40;
    desc_low |= (uint64_t)((limit >> 16) & 0x0F) << 48;
    desc_low |= (uint64_t)(granularity & 0xF0) << 48;
    desc_low |= (uint64_t)((base >> 24) & 0xFF) << 56;
    
    uint64_t desc_high = (base >> 32);
    
    uint64_t *gdt_base = (uint64_t *)gdt;
    gdt_base[index] = desc_low;
    gdt_base[index + 1] = desc_high;
}

void load_gdt() {
    asm volatile (
        "lgdt %0\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        :
        : "m" (gdt_pointer)
        : "rax", "memory"
    );
    asm volatile ("ltr %0" :: "r"((uint16_t)0x28) : "memory");
}

void init_gdt() {
    gdt_pointer.limit = (sizeof(struct gdt_entry) * GDT_ENTRIES) - 1;
    gdt_pointer.base = (uint64_t)&gdt;
    gdt_set_entry(NULL_DESCRIPTOR);
    gdt_set_entry(KERNEL_CODE_SEG);
    gdt_set_entry(KERNEL_DATA_SEG);
    gdt_set_entry(USER_DATA_SEG);
    gdt_set_entry(USER_CODE_SEG);
    memset(&kernel_tss, 0, sizeof(struct tss));
    kernel_tss.rsp0 = (uint64_t)&kernel_stack[4096];
    kernel_tss.iopb_offset = sizeof(struct tss);
    gdt_set_tss(5, (uint64_t)&kernel_tss, sizeof(struct tss) - 1, 0x89, 0x0);
}

void usermod() {
    
}
