#include <stdint.h>
#include <gdt.h>

extern void reload();

// Flat 64-bit GDT store: 8 entries x 8 bytes (null, kernel/user code+data, TSS).
uint8_t gdt_table[8][8] = {0};

// AMD64 task-state segment: rsp0 is the kernel stack used on ring-0 entry.
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved1;
    uint16_t iomap_base;
} __attribute__ ((packed));

// Operand for LGDT: packed so it can be loaded straight from memory.
struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__ ((packed));

// Global for the CPU
static struct tss global_tss = {0};

// Encode a 32-bit-flavored flat segment descriptor into entry `index`.
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

// Encode a 64-bit TSS descriptor across two GDT entries (base high/low).
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

// Fill the static TSS and install its descriptor at GDT index 5.
static inline void write_tss() {
    uint64_t base = (uint64_t)&global_tss;
    uint32_t limit = sizeof(struct tss) - 1;

    global_tss.iomap_base = sizeof(struct tss);
    set_tss_descriptor(5, base, limit, 0x89); // TSS Descriptor
}

// Build the initial GDT (null, kernel/user segments, TSS) and activate it.
void gdt_init() {
    // Layout: null(0), kernel code/data(1,2), user data/code(3,4), then TSS.
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

    // Load the new GDT into the running CPU.
    asm volatile ("lgdt %0" :: "m"(ptr));

    // Load the TSS selector (0x28 = index 5 << 3) so rsp0 switching works.
    uint16_t tss_selector = 0x28;
    asm volatile ("ltr %0" :: "r"(tss_selector));
    // Swap in the kernel data segments and reload CS (defined in reload.asm).
    reload();
}

// Set the kernel stack each process should receive when entering ring 0.
void tss_set_kernel_stack(uintptr_t rsp0) {
    global_tss.rsp0 = rsp0;
}

extern void syscall_entry_stub();

// Read a model-specific register selected in ECX.
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

// Write a model-specific register (value split across EDX:EAX).
static inline void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)(value & 0xFFFFFFFF)),
                  "d"((uint32_t)(value >> 32)) : "memory");
}

// Turn on SYSCALL/SYSRET and wire LSTAR to the assembly entry stub.
void syscall_setup() {
    // EFER.SCE enables SYSCALL; the other writes configure it below.
    uint64_t efer = rdmsr(0xC0000080);
    wrmsr(0xC0000080, efer | 1);
    // STAR: kernel CS 0x08 and user CS 0x08|3 used by SYSCALL/SYSRET.
    wrmsr(0xC0000081, 0x0008000800000000ULL);
    // LSTAR: code address SYSCALL jumps to inside the kernel.
    wrmsr(0xC0000082, (uint64_t)&syscall_entry_stub);
    // SFMASK: RFLAGS bits masked on entry; 0 leaves them untouched.
    wrmsr(0xC0000084, 0);
}