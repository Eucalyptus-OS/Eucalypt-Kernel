#include <syscalls/sys.h>
#include <multitasking/proc.h>
#include <multitasking/sched.h>
#include <stddef.h>
#include <stdint.h>

typedef long (*sysfn)(long a1, long a2, long a3, long a4, long a5);

long sys_fork(void) {
    return proc_fork();
}

long sys_exec(void *elf, uintptr_t size, void *stack) {
    return proc_exec(elf, size, stack);
}

long sys_exit(int code) {
    proc_exit(code);
    return 0;
}

long sys_wait(int *status) {
    struct tcb *cur = get_current_thread();
    struct pcb *me = cur ? cur->parent : NULL;
    if (!me) {
        return -1;
    }
    struct pcb *child = proc_wait(me);
    if (!child) {
        return -1;
    }
    if (status) {
        *status = child->exit_code;
    }
    long pid = (long)child->pid;
    proc_reap(child);
    return pid;
}

long sys_kill(uint64_t pid) {
    struct pcb *child = proc_kill(pid);
    if (!child) {
        return -1;
    }
    proc_reap(child);
    return 0;
}

static long sys_fork_fn(long a1, long a2, long a3, long a4, long a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return sys_fork();
}

static long sys_exec_fn(long a1, long a2, long a3, long a4, long a5) {
    (void)a4; (void)a5;
    return sys_exec((void *)a1, (uintptr_t)a2, (void *)a3);
}

static long sys_exit_fn(long a1, long a2, long a3, long a4, long a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return sys_exit((int)a1);
}

static long sys_wait_fn(long a1, long a2, long a3, long a4, long a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return sys_wait((int *)a1);
}

static long sys_kill_fn(long a1, long a2, long a3, long a4, long a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return sys_kill((uint64_t)a1);
}

static const sysfn syscall_table[SYS_COUNT] = {
    [SYS_FORK] = sys_fork_fn,
    [SYS_EXEC] = sys_exec_fn,
    [SYS_EXIT] = sys_exit_fn,
    [SYS_WAIT] = sys_wait_fn,
    [SYS_KILL] = sys_kill_fn,
};

long syscall_dispatch(int n, long a1, long a2, long a3, long a4, long a5) {
    if (n < 0 || n >= SYS_COUNT || !syscall_table[n]) {
        return -1;
    }
    return syscall_table[n](a1, a2, a3, a4, a5);
}