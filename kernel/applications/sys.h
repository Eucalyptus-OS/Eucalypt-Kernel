#ifndef SYS_H
#define SYS_H

#include <stdint.h>

#define SYSCALL(n, a1, a2, a3) ({ \
    int64_t _ret; \
    __asm__ volatile ( \
        "mov %4, %%r8\n\t" \
        "int $0x80" \
        : "=a"(_ret) \
        : "a"((uint64_t)(n)), "b"((uint64_t)(a1)), "c"((uint64_t)(a2)), "r"((uint64_t)(a3)) \
        : "r8", "memory", "cc" \
    ); \
    _ret; \
})

#define print(s)    SYSCALL(1, s, 0, 0)
#define malloc(sz)  (void*)SYSCALL(2, sz, 0, 0)
#define free(ptr)   SYSCALL(3, ptr, 0, 0)

#endif