#include <syscalls/syscall_input.h>
#include <multitasking/proc.h>
#include <multitasking/sched.h>
#include <logging/printk.h>

uint64_t syscall_input_subscribe(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                  uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

    struct tcb *t = get_current_thread();
    if (!t || !t->parent) {
        return -1;
    }

    uint32_t pid = t->parent->pid;
    input_subscribe(pid);

    return 0;
}

uint64_t syscall_input_unsubscribe(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                                    uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg0; (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

    struct tcb *t = get_current_thread();
    if (!t || !t->parent) {
        return -1;
    }

    uint32_t pid = t->parent->pid;
    input_unsubscribe(pid);

    return 0;
}

uint64_t syscall_input_read(uint64_t arg0, uint64_t arg1, uint64_t arg2,
                             uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg1; (void)arg2; (void)arg3; (void)arg4; (void)arg5;

    struct tcb *t = get_current_thread();
    if (!t || !t->parent) {
        return -1;
    }

    input_event_t *event = (input_event_t *)arg0;
    uint32_t pid = t->parent->pid;

    return input_event_read(pid, event);
}