#include <stdint.h>
#include <logging/printk.h>

uint64_t do_syscall(uint64_t syscall_num, uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    log_debug("Syscall called with number %d and args %llx, %llx, %llx\n", syscall_num, arg0, arg1, arg2);
    switch (syscall_num) {
        case 0:
            log_debug("%s", arg0);
            return 1;
    }
    return 8;
}