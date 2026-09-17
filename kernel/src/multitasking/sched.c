#include <stddef.h>
#include <logging/print.h>
#include <multitasking/thread.h>
#include <multitasking/proc.h>
#include <multitasking/sched.h>
#include <apic.h>

extern void switch_task(struct tcb *t);

// The thread currently executing (or the next to run) on this CPU.
struct tcb *current_tcb = NULL;

// Confirm p is still linked into the live process list before using it.
static int proc_in_list(struct pcb *p) {
    if (!proc_list || !p) {
        return 0;
    }
    struct pcb *r = proc_list;
    do {
        if (r == p) {
            return 1;
        }
        r = r->next;
    } while (r != proc_list);
    return 0;
}

// Destroy exited threads and turn fully-dead processes into zombies/reap them.
static void reap_exited() {
    if (!thread_list) {
        return;
    }

    struct tcb *r = thread_list;
    do {
        struct tcb *next = r->next;
        if (r->state == Exited && r != current_tcb) {
            struct pcb *owner = r->parent;
            int all_exited = 0;
            // Walk the owner's threads: all_exited = every one has finished.
            if (owner) {
                all_exited = 1;
                if (owner->t) {
                    struct tcb *u = owner->t;
                    do {
                        if (u->state != Exited) {
                            all_exited = 0;
                            break;
                        }
                        u = u->proc_next;
                    } while (u != owner->t);
                }
            }
            // Owner was already zombie: leave it for the waiting parent to reap.
            if (owner && all_exited && owner->is_zombie) {
                r = next;
                continue;
            }
            // All threads gone and a live parent exists: turn the owner into a zombie.
            if (owner && all_exited && owner->ppcb && proc_in_list(owner->ppcb) &&
                !owner->ppcb->is_zombie) {
                owner->is_zombie = 1;
                zombie_enqueue(owner);
                r = next;
                continue;
            }
            destroy_thread(r);
            // No threads left at all: release the process outright.
            if (owner && owner->t_count == 0 && owner->t == NULL) {
                proc_destroy(owner);
            }
            if (!thread_list) {
                return;
            }
            r = next;
        } else {
            r = next;
        }
    } while (r != thread_list);
}

// Round-robin scheduler: hand the CPU over to the next Ready thread.
void schedule() {
    uint64_t flags;
    asm volatile ("pushfq; pop %0" : "=r"(flags));
    asm volatile ("cli");

    reap_exited();

    if (!thread_list) {
        print("No threads to switch to\n");
        asm volatile ("push %0; popfq" :: "r"(flags));
        return;
    }

    if (!current_tcb) {
        // Very first switch: just start the first thread found in the Ready state.
        struct tcb *t = thread_list;
        do {
            if (t->state == Ready) {
                t->state = Running;
                print("Switching to first task %d\n", t->tid);
                switch_task(t);
                asm volatile ("push %0; popfq" :: "r"(flags));
                return;
            }
            t = t->next;
        } while (t != thread_list);
        asm volatile ("push %0; popfq" :: "r"(flags));
        return;
    }

    // Fair scan: start looking for a Ready thread just past the current one.
    struct tcb *t = current_tcb->next;
    for (uint64_t i = 0; i < thread_count; i++) {
        if (t->state == Ready) {
            // Put the outgoing thread back on the ready list for its next turn.
            if (current_tcb->state == Running) {
                current_tcb->state = Ready;
            }
            t->state = Running;
            switch_task(t);
            asm volatile ("push %0; popfq" :: "r"(flags));
            return;
        }
        t = t->next;
    }

    asm volatile ("push %0; popfq" :: "r"(flags));
}

// Return the thread that currently owns the CPU.
struct tcb *sched_current_thread() {
    if (!current_tcb) {
        print("Couldn't get the current TCB\n");
        return NULL;
    }
    /* TEST: print("Current TCB\nTID: %d\n", current_tcb->tid); */
    return current_tcb;
}

// Return the process that owns the currently running thread.
struct pcb *sched_current_proc() {
    if (!current_tcb || !current_tcb->parent) {
        print("Couldn't get the current PCB\n");
        return NULL;
    }

    return current_tcb->parent;
}

// Sleep the current thread until somebody explicitly wakes it.
struct tcb *block_current() {
    asm volatile ("cli");
    current_tcb->state = Blocked;

    struct tcb *t = current_tcb;
    schedule();

    // Halt here until the thread is woken and scheduled back onto the CPU.
    while (current_tcb == t && t->state == Blocked) {
        asm volatile ("sti");
        asm volatile ("hlt");
        asm volatile ("cli");
    }
    return t;
}

// Flag t as sleeping; sched_wake_thread() makes it runnable again.
void sched_sleep_thread(struct tcb *t) {
    t->state = Sleeping;
}

// Put t back on the ready list; it will resume on the next scheduling pass.
void sched_wake_thread(struct tcb *t) {
    t->state = Ready;
}

// Thread-safe wake: mark t Ready and restore the caller's interrupt state.
void unblock(struct tcb *t) {
    uint64_t flags;
    asm volatile ("pushfq; pop %0" : "=r"(flags));
    asm volatile ("cli");
    t->state = Ready;
    asm volatile ("push %0; popfq" :: "r"(flags));
}

// Sleep the current thread until system_ticks passes wake_tick.
struct tcb *block_current_timeout(uint64_t wake_tick) {
    asm volatile ("cli");
    current_tcb->state = Blocked;
    current_tcb->timed = 1;
    current_tcb->wake_tick = wake_tick;

    struct tcb *t = current_tcb;
    schedule();

    while (current_tcb == t && t->state == Blocked) {
        asm volatile ("sti");
        asm volatile ("hlt");
        asm volatile ("cli");
    }
    // Deadline reached: clear the timeout flag before returning.
    current_tcb->timed = 0;
    return t;
}

// Wake every blocked thread whose wake_tick has now been reached (timer tick).
void sched_check_timeouts() {
    if (!thread_list) {
        return;
    }
    struct tcb *r = thread_list;
    do {
        struct tcb *next = r->next;
        if (r->state == Blocked && r->timed && system_ticks >= r->wake_tick) {
            r->timed = 0;
            unblock(r);
        }
        r = next;
    } while (r != thread_list);
}