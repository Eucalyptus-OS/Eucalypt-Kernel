#ifndef SYS_H
#define SYS_H
#include <stdint.h>

typedef struct {
    char name[13];
    uint32_t size;
    uint8_t attr;
} file_info_t;

#define SYSCALL(n, a1, a2, a3) ({ \
    int64_t _ret; \
    __asm__ volatile ( \
        "int $0x80" \
        : "=a"(_ret) \
        : "a"((uint64_t)(n)), "b"((uint64_t)(a1)), "c"((uint64_t)(a2)), "d"((uint64_t)(a3)) \
        : "memory", "cc" \
    ); \
    _ret; \
})

#define print(s)                    SYSCALL(1, s, 0, 0)
#define malloc(sz)                  (void*)SYSCALL(2, sz, 0, 0)
#define free(ptr)                   SYSCALL(3, ptr, 0, 0)
#define write_file(f, d)            SYSCALL(4, f, d, 0)
#define read_file(f, out_size_ptr)  (uint8_t*)SYSCALL(5, f, out_size_ptr, 0)
#define exec(f)                     SYSCALL(6, f, 0, 0)
#define ls(buf_ptr, max_entries)    SYSCALL(7, buf_ptr, max_entries, 0)
#define sleep(ms)                   SYSCALL(8, ms, 0, 0)

#endif