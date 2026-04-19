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

static inline uint32_t *get_framebuffer(void) { return (uint32_t *)syscall(1, 0, 0, 0); }
static inline uint64_t  fb_pitch(void)        { return syscall(2, 2, 0, 0); }
static inline uint8_t   tty_write(char *msg, uint64_t len) {return syscall(4, (uint64_t)msg, len, 0); }

void _start(void) {
    uint32_t *fb    = get_framebuffer();
    uint64_t  pitch = fb_pitch();
    uint64_t  stride = pitch / 4;

    if (!fb) while (1) {}

    for (uint64_t y = 50; y < 250; y++)
        for (uint64_t x = 50; x < 250; x++)
            fb[y * stride + x] = 0xFFFFFF;
    
    char *msg = "Hello world!";
    tty_write(msg, 13);

    while (1) {}
}