#include <stdint.h>
#include <stdbool.h>
#include <portio.h>
#include <interrupts/apic.h>
#include <logging/printk.h>
#include <panic.h>
#include <mm/paging.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <multitasking/sched.h>
#include <multitasking/proc.h>
#include <ipc/signal.h>
#include <smp.h>
#include <idt/idt.h>

extern void apic_handler();
extern void int128_handler();
extern void ps2_keyboard_handler();
extern void ps2_mouse_handler();
extern void tlb_shootdown_handler();

idt_per_cpu_t *idt_per_cpu_data[100];

__attribute__((aligned(0x10)))
static idt_entry_t idt[256];
static idtr_t idtr;

extern void *isr_stub_table[];

static const char *exception_name(uint64_t vector) {
    static const char *names[] = {
        "Divide error",
        "Debug",
        "NMI",
        "Breakpoint",
        "Overflow",
        "Bound range",
        "Invalid opcode",
        "Device unavailable",
        "Double fault",
        "Coprocessor segment overrun",
        "Invalid TSS",
        "Segment not present",
        "Stack fault",
        "General protection",
        "Page fault",
        "Reserved",
        "x87 floating-point",
        "Alignment check",
        "Machine check",
        "SIMD floating-point",
        "Virtualization",
        "Control protection",
    };

    if (vector < sizeof(names) / sizeof(names[0]) && names[vector])
        return names[vector];
    return "Unknown";
}

static bool canonical_addr(uint64_t addr) {
    return ((int64_t)(addr << 16) >> 16) == (int64_t)addr;
}

static bool fault_read_u8(uint64_t *pml4, uint64_t addr, uint8_t *out) {
    if (!canonical_addr(addr))
        return false;

    uint64_t entry = paging_get_entry(pml4, addr & ~0xFFFULL);
    if (!(entry & ENTRY_FLAG_PRESENT))
        return false;

    uint8_t *src = (uint8_t *)(offset + (entry & ENTRY_4K_ADDRESS_MASK) + (addr & 0xFFF));
    *out = *src;
    return true;
}

static bool fault_read_u64(uint64_t *pml4, uint64_t addr, uint64_t *out) {
    uint64_t value = 0;

    for (int i = 0; i < 8; i++) {
        uint8_t byte;
        if (!fault_read_u8(pml4, addr + (uint64_t)i, &byte))
            return false;
        value |= (uint64_t)byte << (i * 8);
    }

    *out = value;
    return true;
}

static void dump_page_fault_error(uint64_t error) {
    log_fatal("PF error: present=%llu write=%llu user=%llu reserved=%llu instruction=%llu pk=%llu shadow_stack=%llu sgx=%llu",
              error & 1,
              (error >> 1) & 1,
              (error >> 2) & 1,
              (error >> 3) & 1,
              (error >> 4) & 1,
              (error >> 5) & 1,
              (error >> 6) & 1,
              (error >> 15) & 1);
}

static void dump_code_bytes(uint64_t *pml4, uint64_t rip) {
    uint8_t b[16];
    bool ok[16];

    for (int i = 0; i < 16; i++)
        ok[i] = fault_read_u8(pml4, rip + (uint64_t)i, &b[i]);

    log_fatal("Code at RIP:");
    for (int i = 0; i < 16; i++) {
        if (ok[i])
            log_fatal("  RIP+%02d: %02X", i, b[i]);
        else
            log_fatal("  RIP+%02d: <unmapped>", i);
    }
}

static void dump_stack_qwords(uint64_t *pml4, uint64_t rsp) {
    log_fatal("Stack qwords at saved RSP=%#018llx:", (unsigned long long)rsp);

    for (int i = 0; i < 12; i++) {
        uint64_t addr = rsp + (uint64_t)i * 8;
        uint64_t val;
        if (fault_read_u64(pml4, addr, &val)) {
            log_fatal("  [%#018llx] = %#018llx",
                      (unsigned long long)addr,
                      (unsigned long long)val);
        } else {
            log_fatal("  [%#018llx] = <unmapped>", (unsigned long long)addr);
            break;
        }
    }
}

static void dump_backtrace(uint64_t *pml4, uint64_t rip, uint64_t rbp) {
    log_fatal("RBP backtrace:");
    log_fatal("  #0 rip=%#018llx", (unsigned long long)rip);

    for (int i = 1; i < 16; i++) {
        uint64_t next_rbp, ret;

        if (!fault_read_u64(pml4, rbp, &next_rbp) ||
            !fault_read_u64(pml4, rbp + 8, &ret)) {
            log_fatal("  #%d rbp=%#018llx <unmapped>",
                      i, (unsigned long long)rbp);
            break;
        }

        log_fatal("  #%d rbp=%#018llx rip=%#018llx",
                  i,
                  (unsigned long long)rbp,
                  (unsigned long long)ret);

        if (next_rbp <= rbp || next_rbp - rbp > 0x100000)
            break;
        rbp = next_rbp;
    }
}

static void dump_exception_frame(interrupt_frame_t *f, uint64_t cr2, uint64_t cr3) {
    int32_t pid = get_current_pid();
    struct tcb *thread = get_current_thread();

    log_fatal("FAULT DUMP: vector=%llu (%s) error=%#llx tid=%d pid=%d",
              (unsigned long long)f->vector,
              exception_name(f->vector),
              (unsigned long long)f->error_code,
              thread ? thread->tid : -1,
              pid);
    log_fatal("Saved frame: RIP=%#018llx CS=%#06llx RFLAGS=%#018llx RSP=%#018llx SS=%#06llx",
              (unsigned long long)f->rip,
              (unsigned long long)f->cs,
              (unsigned long long)f->rflags,
              (unsigned long long)f->rsp,
              (unsigned long long)f->ss);
    log_fatal("CR2=%#018llx CR3=%#018llx",
              (unsigned long long)cr2,
              (unsigned long long)cr3);

    if (f->vector == 14)
        dump_page_fault_error(f->error_code);

    log_fatal("Regs: RAX=%#018llx RBX=%#018llx RCX=%#018llx RDX=%#018llx",
              (unsigned long long)f->rax,
              (unsigned long long)f->rbx,
              (unsigned long long)f->rcx,
              (unsigned long long)f->rdx);
    log_fatal("Regs: RSI=%#018llx RDI=%#018llx RBP=%#018llx",
              (unsigned long long)f->rsi,
              (unsigned long long)f->rdi,
              (unsigned long long)f->rbp);
    log_fatal("Regs: R8 =%#018llx R9 =%#018llx R10=%#018llx R11=%#018llx",
              (unsigned long long)f->r8,
              (unsigned long long)f->r9,
              (unsigned long long)f->r10,
              (unsigned long long)f->r11);
    log_fatal("Regs: R12=%#018llx R13=%#018llx R14=%#018llx R15=%#018llx",
              (unsigned long long)f->r12,
              (unsigned long long)f->r13,
              (unsigned long long)f->r14,
              (unsigned long long)f->r15);

    if (!offset) {
        log_fatal("HHDM offset is not initialized; skipping code, stack, and backtrace memory reads");
        return;
    }

    uint64_t *pml4 = (uint64_t *)(offset + (cr3 & ENTRY_4K_ADDRESS_MASK));
    dump_code_bytes(pml4, f->rip);
    dump_stack_qwords(pml4, f->rsp);
    dump_backtrace(pml4, f->rip, f->rbp);
}

void idt_set_descriptor(uint8_t vector, void *isr, uint8_t flags) {
    idt_entry_t *d = &idt[vector];
    d->isr_low   = (uint64_t)isr & 0xFFFF;
    d->kernel_cs = 0x08;
    d->ist       = 0;
    d->attributes = flags;
    d->isr_mid   = ((uint64_t)isr >> 16) & 0xFFFF;
    d->isr_high  = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    d->reserved  = 0;
}

void idt_init() {
    idtr.base  = (uintptr_t)&idt[0];
    idtr.limit = sizeof(idt_entry_t) * 256 - 1;

    for (uint16_t v = 0; v < 256; v++)
        idt_set_descriptor((uint8_t)v, isr_stub_table[v], 0x8E);

    idt_set_descriptor(APIC_TIMER_VECTOR, apic_handler,   0x8E);
    idt_set_descriptor(SYSCALL_VECTOR,    int128_handler, 0xEE);
    idt_set_descriptor(PS2_KEYBOARD_VECTOR, ps2_keyboard_handler, 0x8E);
    idt_set_descriptor(PS2_MOUSE_VECTOR, ps2_mouse_handler, 0x8E);
    idt_set_descriptor(TLB_SHOOTDOWN_VECTOR, tlb_shootdown_handler, 0x8E);

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    __asm__ volatile ("lidt %0" :: "m"(idtr));
}

void idt_init_per_cpu() {
    idtr_t local_idtr = {
        .base  = (uintptr_t)&idt[0],
        .limit = sizeof(idt_entry_t) * 256 - 1,
    };
    __asm__ volatile ("lidt %0" :: "m"(local_idtr));
}

void exit_syscall(interrupt_frame_t *f) {
    struct pcb *proc = proc_get(get_current_pid());
    if (!proc) {
        return;
    }
    if (!proc->signal_pending) {
        return;
    }
    for (int i = 0; i < NSIG; i++) {
        if (!(proc->signal_pending & (1U << i))) {
            continue;
        }
        proc->signal_pending &= ~(1U << i);
        if (i == SIGKILL || i == SIGSTOP) {
            default_sig_handler(i);
        } else {
            f->rip = (uint64_t)proc->signal_handler[i];
            f->rdi = i;
        }
    }
}

static void exception_handler(interrupt_frame_t *f) {
    __asm__ volatile ("cli");
    uint64_t cr2 = 0, cr3 = 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

    dump_exception_frame(f, cr2, cr3);

    panic("Exception %u, error %u, RIP=%#018llx, CR2=%#018llx, CR3=%#018llx\n",
          (unsigned)f->vector,
          (unsigned)f->error_code,
          (unsigned long long)f->rip,
          (unsigned long long)cr2,
          (unsigned long long)cr3);
    for (;;) __asm__ volatile ("cli\nhlt");
}

uint64_t apic_interrupt(uint64_t rsp) {
    apic_eoi();
    if (apic_id() == bsp_lapic_id) {
        printk_drain_tick();
    }
    return schedule(rsp);
}

uintptr_t isr_handler(interrupt_frame_t *f) {
    exception_handler(f);
    return (uintptr_t)f;
}
