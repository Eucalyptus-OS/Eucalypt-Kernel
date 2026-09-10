#pragma once

#include <stdint.h>

enum {
    SYS_FORK,
    SYS_EXEC,
    SYS_EXIT,
    SYS_WAIT,
    SYS_KILL,
    SYS_COUNT
};

long syscall_dispatch(int n, long a1, long a2, long a3, long a4, long a5);
long sys_fork(void);
long sys_exec(void *elf, uintptr_t size, void *stack);
long sys_exit(int code);
long sys_wait(int *status);
long sys_kill(uint64_t pid);