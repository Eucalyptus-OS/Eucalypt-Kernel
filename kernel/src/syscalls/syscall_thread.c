#include <logging/printk.h>
#include <multitasking/sched.h>
#include <multitasking/proc.h>
#include <multitasking/thread.h>
#include <syscalls/syscall_thread.h>

uint64_t sys_create_thread(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    struct pcb *pcb = proc_get(get_current_pid());
    if (!pcb) {
        return -1;
    }

    struct tcb *tcb = create_user_thread(a, pcb->cr3);
    if (!tcb) {
        return -1;
    }

    int i = 0;
    
    while (pcb->threads[i] != NULL) {
        i++;
    }

    pcb->threads[i] = tcb;

    return (uint64_t)0;
}

uint64_t sys_thread_exit(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    (void)b; (void)c; (void)d; (void)e; (void)f;

    struct tcb *tcb = get_current_thread();
    if (!tcb) {
        return -1;
    }

    log_info("Thread %d exited with code %ld\n", tcb->tid, (int64_t)a);
    tcb->state = dead;

    return (uint64_t)0;
}
