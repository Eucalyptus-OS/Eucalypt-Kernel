#include <stdint.h>
#include <flanterm/flanterm.h>
#include <x86_64/serial.h>

extern struct flanterm_context *ft_ctx;

#define SYSCALL_NULL 0
#define SYSCALL_WRITE 1

int syscall_handler(uint8_t syscall, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    switch (syscall) {
        case SYSCALL_NULL:
            return 0;
        case SYSCALL_WRITE:
            serial_print("Sys write\n");
            serial_print((const char *)arg1);
            serial_print("^arg1^");
            flanterm_write(ft_ctx, (const char *)arg1);
            return 0;
        default:
            return -1;
    }
}