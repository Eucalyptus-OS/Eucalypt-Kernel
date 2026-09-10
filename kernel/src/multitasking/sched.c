#include <multitasking/sched.h>
#include <multitasking/thread.h>
#include <multitasking/proc.h>
#include <mm/heap.h>
#include <mm/frame.h>
#include <mm/hhdm.h>
#include <logging/print.h>
#include <sync/spinlock.h>
#include <stddef.h>
#include <stdint.h>

extern void switch_task(struct tcb *t);

spinlock_t sched_lock = 0;

struct tcb *current_tcb = NULL;

volatile uint32_t preempt_depth = 0;

#define THREAD_STACK_SIZE 4096

static void reap_thread(struct tcb *t) {
    if (t->parent) {
        struct tcb *pc = NULL;
        struct tcb *pt = t->parent->threads;
        while (pt && pt != t) {
            pc = pt;
            pt = pt->pthread_next;
        }
        if (pt == t) {
            if (pc) {
                pc->pthread_next = t->pthread_next;
            } else {
                t->parent->threads = t->pthread_next;
            }
            if (t->parent->t_count) {
                t->parent->t_count--;
            }
        }
    }
    frame_free(virt_to_phys(t->kstack_top - THREAD_STACK_SIZE));
    kfree(t);
}

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
    struct tcb *ready = NULL;

    // Bounded by the pre-reap count: strips Dead nodes while hunting for a
    // Ready one. Removals only shrink the circular list, so bounds iterations
    // visit every distinct node. The scan never touches current_tcb itself
    // (the `next != current_tcb` guard), so an exiting thread's stack is
    // always safe until it has actually switched away.
    uint64_t bounds = thread_count;
    for (uint64_t i = 0; i < bounds && ready == NULL && next != NULL; i++) {
        if (next->state == Dead && next != current_tcb) {
            struct tcb *victim = next;
            if (prev) {
                prev->next = next->next;
            } else {
                thread_list = next->next;
            }
            if (next->next == next) {
                thread_list = NULL;
                next = NULL;
            } else {
                next = next->next;
            }
            thread_count--;
            reap_thread(victim);
            continue;
        }
        if (next->state == Ready) {
            ready = next;
            break;
        }
        prev = next;
        next = next->next;
    }

    if (!ready) {
        spinlock_release_irqrestore(&sched_lock, flags);
        return;
    }

    ready->state = Running;
    if (prev && prev->state == Running) {
        prev->state = Ready;
    }

    spinlock_release(&sched_lock);
    switch_task(ready);
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