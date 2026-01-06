#ifndef SYS_H
#define SYS_H
#include <stdint.h>

typedef struct {
    char name[13];
    uint32_t size;
    uint8_t attr;
} file_info_t;

static inline int64_t syscall(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(n), "b"(a1), "c"(a2), "d"(a3)
        : "memory", "cc"
    );
    return ret;
}

static inline int64_t print(const char *s) {
    return syscall(1, (uint64_t)s, 0, 0);
}

static inline void* malloc(uint64_t sz) {
    return (void*)syscall(2, sz, 0, 0);
}

static inline int64_t free(void *ptr) {
    return syscall(3, (uint64_t)ptr, 0, 0);
}

static inline int64_t write_file(const char *f, const uint8_t *d) {
    return syscall(4, (uint64_t)f, (uint64_t)d, 0);
}

static inline uint8_t* read_file(const char *f, uint32_t *out_size_ptr) {
    return (uint8_t*)syscall(5, (uint64_t)f, (uint64_t)out_size_ptr, 0);
}

static inline int64_t exec(const char *f) {
    return syscall(6, (uint64_t)f, 0, 0);
}

static inline int64_t ls(file_info_t *buf_ptr, uint64_t max_entries) {
    return syscall(7, (uint64_t)buf_ptr, max_entries, 0);
}

static inline int64_t sleep(uint64_t ms) {
    return syscall(8, ms, 0, 0);
}

#endif