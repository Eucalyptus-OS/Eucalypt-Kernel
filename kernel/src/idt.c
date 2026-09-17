#include <stdint.h>
#include <stdbool.h>
#include <limine.h>
#include <logging/print.h>
#include <apic.h>
#include <mm/page.h>
#include <signal.h>
#include <multitasking/sched.h>
#include <multitasking/thread.h>
#include <multitasking/proc.h>
#include <tty.h>
#include <idt.h>

#define IDT_MAX_DESCRIPTORS 256

extern void* isr_stub_table[];
extern void apic_stub();
extern void ahci_stub();
extern void keyboard_stub();
extern void int128_handler();

extern volatile struct limine_framebuffer_request framebuffer_request;

typedef struct {
    uint16_t    isr_low;
    uint16_t    kernel_cs;
    uint8_t     ist;
    uint8_t     attributes;
    uint16_t    isr_mid;
    uint32_t    isr_high;
    uint32_t    reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t    limit;
    uint64_t    base;
} __attribute__((packed)) idtr_t;

__attribute__((aligned(0x10)))
static idt_entry_t idt[IDT_MAX_DESCRIPTORS];

static idtr_t idtr;

static bool vectors[IDT_MAX_DESCRIPTORS];

static const char* exception_names[32] = {
    "Divide by Zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 Floating-Point Exception",
    "Alignment Check", "Machine Check", "SIMD Floating-Point Exception",
    "Virtualization Exception", "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Hypervisor Injection Exception", "VMM Communication Exception",
    "Security Exception", "Reserved"
};

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    idt_entry_t* descriptor = &idt[vector];

    descriptor->isr_low        = (uint64_t)isr & 0xFFFF;
    descriptor->kernel_cs      = 0x08;
    descriptor->ist            = 0;
    descriptor->attributes     = flags;
    descriptor->isr_mid        = ((uint64_t)isr >> 16) & 0xFFFF;
    descriptor->isr_high       = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    descriptor->reserved       = 0;

    vectors[vector] = true;
    print("IDT entry added\nVector: %d\nISR: %X\nFlags: %X", vector, isr, flags);
}

void idt_init() {
    idtr.base = (uintptr_t)&idt[0];
    idtr.limit = (uint16_t)(sizeof(idt_entry_t) * IDT_MAX_DESCRIPTORS - 1);

    for (uint8_t vector = 0; vector < 32; vector++) {
        idt_set_descriptor(vector, isr_stub_table[vector], 0x8E);
    }

    idt_set_descriptor(0x20, apic_stub, 0x8E);
    idt_set_descriptor(0x21, ahci_stub, 0x8E);
    idt_set_descriptor(0x22, keyboard_stub, 0x8E);
    idt_set_descriptor(0x80, int128_handler, 0xEE);

    __asm__ volatile ("lidt %0" : : "m"(idtr));
    print("IDT loaded");
}

void exception_handler(uint64_t vector, uint64_t error_code, user_context_t *ctx) {
    if (ctx && (ctx->cs & 3) == 3) {
        int sig = 0;
        switch (vector) {
            case 0:
            case 16:
            case 19:
                sig = SIGFPE;
                break;
            case 3:
                sig = SIGTRAP;
                break;
            case 6:
                sig = SIGILL;
                break;
            case 17:
                sig = SIGBUS;
                break;
            case 4:
            case 5:
            case 8:
            case 10:
            case 11:
            case 12:
            case 13:
            case 14:
                sig = SIGSEGV;
                break;
            default:
                break;
        }
        if (sig) {
            struct pcb *p = sched_current_proc();
            uint64_t cr2_v = 0;
            asm volatile ("mov %%cr2, %0" : "=r"(cr2_v));
            uint64_t msr_fs = 0;
            asm volatile ("rdmsr" : "=a"(msr_fs), "=d"(msr_fs) : "c"(0xC0000100));
            print("USERFAULT pid=%d vector=%lu rip=%lx rbx=%lx cr2=%lx fs_tcb=%lx fs_msr=%lx sig=%d\n",
                  p ? p->pid : -1, (unsigned long)vector, (unsigned long)ctx->rip,
                  (unsigned long)ctx->rbx, (unsigned long)cr2_v,
                  (unsigned long)current_tcb->fs_base, (unsigned long)(msr_fs & 0xFFFFFFFFFFFFFFFF), sig);
            {
                unsigned char *ip = (unsigned char *)(uintptr_t)ctx->rip;
                print("FAULTBYTES pre");
                for (int i = -16; i < 0; i++) {
                    print(" %02x", (unsigned int)ip[i]);
                }
                print(" post");
                for (int i = 0; i < 24; i++) {
                    print(" %02x", (unsigned int)ip[i]);
                }
                print(" rcx=%lx rdx=%lx rsi=%lx rdi=%lx r8=%lx r9=%lx r10=%lx r11=%lx rax=%lx rbp=%lx rsp=%lx r12=%lx r13=%lx r14=%lx r15=%lx\n",
                      (unsigned long)ctx->rcx, (unsigned long)ctx->rdx,
                      (unsigned long)ctx->rsi, (unsigned long)ctx->rdi,
                      (unsigned long)ctx->r8, (unsigned long)ctx->r9,
                      (unsigned long)ctx->r10, (unsigned long)ctx->r11,
                      (unsigned long)ctx->rax, (unsigned long)ctx->rbp,
                      (unsigned long)ctx->rsp, (unsigned long)ctx->r12,
                      (unsigned long)ctx->r13, (unsigned long)ctx->r14,
                      (unsigned long)ctx->r15);
            }
            if (p) {
                sigset_t bit = (sigset_t)1 << (sig - 1);
                if (p->sigstate.blocked & bit) {
                    p->sigstate.pending |= bit;
                } else {
                    sig_deliver(p, sig, ctx, ctx->rax);
                }
            }
        }
        sig_deliver_current(ctx, ctx->rax);
        return;
    }

    uint64_t cr2, cr3, cr4, rflags, cs, ss;
    uint64_t rip = ctx ? ctx->rip : 0;

    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile ("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile ("mov %%ss, %0" : "=r"(ss));

    const char* name = "Unknown";
    if (vector < 32) {
        name = exception_names[vector];
    }

    print("\n================================================================\n");
    print("                        KERNEL PANIC\n");
    print("================================================================\n");
    print("Exception : %s (vector %lu)\n", name, (unsigned long)vector);
    print("Error Code: 0x%016lx\n", (unsigned long)error_code);
    print("RIP       : 0x%016lx\n", (unsigned long)rip);
    print("CR2       : 0x%016lx\n", (unsigned long)cr2);
    print("CR3       : 0x%016lx\n", (unsigned long)cr3);
    print("CR4       : 0x%016lx\n", (unsigned long)cr4);
    print("RFLAGS    : 0x%016lx\n", (unsigned long)rflags);
    print("CS        : 0x%04lx\n",  (unsigned long)cs);

    {
        uint64_t pml4i = (cr2 >> 39) & 0x1FF;
        uint64_t pdpti = (cr2 >> 30) & 0x1FF;
        uint64_t pdi = (cr2 >> 21) & 0x1FF;
        uint64_t pti = (cr2 >> 12) & 0x1FF;
        uint64_t *pml4 = (uint64_t *)(cr3 + 0xffff800000000000ULL);
        uint64_t pml4e = pml4[pml4i];
        uint64_t *pdpt = (uint64_t *)((pml4e & 0x000FFFFFFFFFF000ULL) + 0xffff800000000000ULL);
        uint64_t pdpte = pml4e & 1 ? pdpt[pdpti] : 0;
        uint64_t *pd = (uint64_t *)((pdpte & 0x000FFFFFFFFFF000ULL) + 0xffff800000000000ULL);
        uint64_t pde = pdpte & 1 ? pd[pdi] : 0;
        uint64_t *pt = (uint64_t *)((pde & 0x000FFFFFFFFFF000ULL) + 0xffff800000000000ULL);
        uint64_t pte = pde & 1 ? pt[pti] : 0;
        print("PAGEWALK i4=%lu[%lx] i3=%lu[%lx] i2=%lu[%lx] i1=%lu[%lx]\n",
              (unsigned long)pml4i, (unsigned long)pml4e,
              (unsigned long)pdpti, (unsigned long)pdpte,
              (unsigned long)pdi, (unsigned long)pde,
              (unsigned long)pti, (unsigned long)pte);
        extern volatile struct limine_framebuffer_request framebuffer_request;
        if (framebuffer_request.response && framebuffer_request.response->framebuffer_count > 0) {
            print("FBSTATE resp=%lx addr=%lx pitch=%u width=%u height=%u\n",
                  (unsigned long)framebuffer_request.response,
                  (unsigned long)framebuffer_request.response->framebuffers[0]->address,
                  (unsigned)framebuffer_request.response->framebuffers[0]->pitch,
                  (unsigned)framebuffer_request.response->framebuffers[0]->width,
                  (unsigned)framebuffer_request.response->framebuffers[0]->height);
        }
        extern tty_t *tty_get_active();
        tty_t *atty = tty_get_active();
        if (atty) {
            print("TTYSTATE row=%u col=%u max_rows=%u max_cols=%u origin_y=%u origin_x=%u\n",
                  (unsigned)atty->row, (unsigned)atty->col,
                  (unsigned)atty->max_rows, (unsigned)atty->max_cols,
                  (unsigned)atty->origin_y, (unsigned)atty->origin_x);
        }
    }
    print("SS        : 0x%04lx\n",  (unsigned long)ss);
    print("----------------------------------------------------------------\n");
    print("System halted.\n");
    print("================================================================\n");
    for (;;) {
        asm volatile ("hlt");
    }
}