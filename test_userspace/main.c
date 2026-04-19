#include <stdint.h>

static uint64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    uint64_t ret;
    asm volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "D" (arg1), "S" (arg2), "d" (arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline void plot_point(uint64_t x, uint64_t y) {
    syscall(0, x, y, 0xFFFFFF);
}

void _start(void) {
    plot_point(700, 700);
    while (1) {}
}