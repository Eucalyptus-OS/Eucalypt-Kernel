#include <multitasking/sched.h>
#include <multitasking/thread.h>
#include <logging/print.h>
#include <sync/spinlock.h>
#include <stddef.h>

extern void switch_task(struct tcb *t);

spinlock_t sched_lock = 0;

struct tcb *current_tcb = NULL;

volatile uint32_t preempt_depth = 0;

void schedule() {
    if (__atomic_load_n(&preempt_depth, __ATOMIC_RELAXED)) {
        return;
    }

    uint64_t flags = spinlock_acquire_irqsave(&sched_lock);

    if (!thread_list) {
        spinlock_release_irqrestore(&sched_lock, flags);
        return;
    }

    struct tcb *prev = current_tcb;
    struct tcb *next = prev ? prev->next : thread_list;

    for (uint64_t i = 0; i < thread_count; i++) {
        if (next->state == Ready) {
            break;
        }
        next = next->next;
    }

    if (next->state != Ready) {
        spinlock_release_irqrestore(&sched_lock, flags);
        return;
    }

    next->state = Running;
    if (prev && prev->state == Running) {
        prev->state = Ready;
    }

    spinlock_release(&sched_lock);
    switch_task(next);
    restore_irq(flags);
}

struct tcb *get_current_thread() {
    if (!current_tcb) {
        return NULL;
    }

    return current_tcb;
}

struct tcb *block_current() {
    asm volatile ("cli");
    current_tcb->state = Blocked;
    asm volatile ("sti");

    struct tcb *t = current_tcb;
    print("Blocking %d\n", t->tid);
    schedule();

    while (current_tcb == t && t->state == Blocked) {
        asm volatile ("sti");
        asm volatile ("hlt");
    }
    return t;
}

void sched_sleep_thread(struct tcb *t) {
    t->state = Sleeping;
    print("Sleeping thread: %d\n", t->tid);
}

void sched_wake_thread(struct tcb *t) {
    t->state = Ready;
    print("Waking thread: %d\n", t->tid);
}

void unblock(struct tcb *t) {
    asm volatile ("cli");
    t->state = Ready;
    asm volatile ("sti");
    print("Unblocking %d\n", t->tid);
}