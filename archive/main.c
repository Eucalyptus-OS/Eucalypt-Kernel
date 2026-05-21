#include <stdint.h>

// Match whatever your kernel's do_syscall dispatch expects
#define SYS_WRITE 1

static uint64_t syscall(uint64_t num, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    uint64_t ret;
    __asm__ volatile (
        "int $0x80\n"
        : "=a" (ret)
        : "a" (num), "D" (arg0), "S" (arg1), "d" (arg2)
        : "memory"
    );
    return ret;
}

static const char msg[] = "Hello world!\n";

void _start(void) {
    syscall(SYS_WRITE, 1, (uint64_t)msg, sizeof(msg) - 1);
    for (;;) __asm__ volatile ("nop");
}