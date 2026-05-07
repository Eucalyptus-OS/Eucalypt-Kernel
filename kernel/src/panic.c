#include <stdint.h>
#include <stdarg.h>
#include <logging/printk.h>
#include <assert.h>

typedef struct [[gnu::packed]] {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cs, ss, ds, es, fs, gs;
} register_dump_t;

static register_dump_t panic_regs;

[[gnu::noreturn]] void panic(const char *fmt, ...) {
    __asm__ volatile (
        "mov %%rax, %0\n"
        "mov %%rbx, %1\n"
        "mov %%rcx, %2\n"
        "mov %%rdx, %3\n"
        "mov %%rsi, %4\n"
        "mov %%rdi, %5\n"
        "mov %%rbp, %6\n"
        "mov %%rsp, %7\n"
        "mov %%r8,  %8\n"
        "mov %%r9,  %9\n"
        "mov %%r10, %10\n"
        "mov %%r11, %11\n"
        "mov %%r12, %12\n"
        "mov %%r13, %13\n"
        "mov %%r14, %14\n"
        "mov %%r15, %15\n"
        "pushfq\n"
        "pop %16\n"
        "xor %%rax, %%rax\n"
        "mov %%cs, %%ax\n"  "mov %%rax, %18\n"
        "mov %%ss, %%ax\n"  "mov %%rax, %19\n"
        "mov %%ds, %%ax\n"  "mov %%rax, %20\n"
        "mov %%es, %%ax\n"  "mov %%rax, %21\n"
        "mov %%fs, %%ax\n"  "mov %%rax, %22\n"
        "mov %%gs, %%ax\n"  "mov %%rax, %23\n"
        : "=m"(panic_regs.rax), "=m"(panic_regs.rbx),
          "=m"(panic_regs.rcx), "=m"(panic_regs.rdx),
          "=m"(panic_regs.rsi), "=m"(panic_regs.rdi),
          "=m"(panic_regs.rbp), "=m"(panic_regs.rsp),
          "=m"(panic_regs.r8),  "=m"(panic_regs.r9),
          "=m"(panic_regs.r10), "=m"(panic_regs.r11),
          "=m"(panic_regs.r12), "=m"(panic_regs.r13),
          "=m"(panic_regs.r14), "=m"(panic_regs.r15),
          "=m"(panic_regs.rflags), "=m"(panic_regs.rip),
          "=m"(panic_regs.cs),  "=m"(panic_regs.ss),
          "=m"(panic_regs.ds),  "=m"(panic_regs.es),
          "=m"(panic_regs.fs),  "=m"(panic_regs.gs)
        :
        : "rax"
    );

    __asm__ volatile (
        "lea (%%rip), %0\n"
        : "=r"(panic_regs.rip)
    );

va_list ap;
    va_start(ap, fmt);
    vprintk_level(LOG_FATAL, fmt, ap);
    va_end(ap);

    printk_level(LOG_FATAL, "RAX=%#018llX  RBX=%#018llX  RCX=%#018llX  RDX=%#018llX",
        (unsigned long long)panic_regs.rax, (unsigned long long)panic_regs.rbx,
        (unsigned long long)panic_regs.rcx, (unsigned long long)panic_regs.rdx);

    printk_level(LOG_FATAL, "RSI=%#018llX  RDI=%#018llX  RBP=%#018llX  RSP=%#018llX",
        (unsigned long long)panic_regs.rsi, (unsigned long long)panic_regs.rdi,
        (unsigned long long)panic_regs.rbp, (unsigned long long)panic_regs.rsp);

    printk_level(LOG_FATAL, "R8 =%#018llX  R9 =%#018llX  R10=%#018llX  R11=%#018llX",
        (unsigned long long)panic_regs.r8,  (unsigned long long)panic_regs.r9,
        (unsigned long long)panic_regs.r10, (unsigned long long)panic_regs.r11);

    printk_level(LOG_FATAL, "R12=%#018llX  R13=%#018llX  R14=%#018llX  R15=%#018llX",
        (unsigned long long)panic_regs.r12, (unsigned long long)panic_regs.r13,
        (unsigned long long)panic_regs.r14, (unsigned long long)panic_regs.r15);

    printk_level(LOG_FATAL, "RIP=%#018llX  RFLAGS=%#018llX",
        (unsigned long long)panic_regs.rip,
        (unsigned long long)panic_regs.rflags);

    printk_level(LOG_FATAL, "CS=%#06llX  SS=%#06llX  DS=%#06llX  ES=%#06llX  FS=%#06llX  GS=%#06llX",
        (unsigned long long)panic_regs.cs,  (unsigned long long)panic_regs.ss,
        (unsigned long long)panic_regs.ds,  (unsigned long long)panic_regs.es,
        (unsigned long long)panic_regs.fs,  (unsigned long long)panic_regs.gs);

    for (;;) {
        __asm__ volatile ("cli\nhlt");
    }
}