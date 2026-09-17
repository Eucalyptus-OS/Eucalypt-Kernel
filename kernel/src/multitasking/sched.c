#include <stddef.h>
#include <logging/print.h>
#include <multitasking/thread.h>
#include <multitasking/proc.h>
#include <multitasking/sched.h>
#include <apic.h>

extern void switch_task(struct tcb *t);

struct tcb *current_tcb = NULL;

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
            if (owner && all_exited && owner->is_zombie) {
                r = next;
                continue;
            }
            if (owner && all_exited && owner->ppcb && proc_in_list(owner->ppcb) &&
                !owner->ppcb->is_zombie) {
                owner->is_zombie = 1;
                zombie_enqueue(owner);
                r = next;
                continue;
            }
            destroy_thread(r);
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

    struct tcb *t = current_tcb->next;
    for (uint64_t i = 0; i < thread_count; i++) {
        if (t->state == Ready) {
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

struct tcb *sched_current_thread() {
    if (!current_tcb) {
        print("Couldn't get the current TCB\n");
        return NULL;
    }
    /* TEST: print("Current TCB\nTID: %d\n", current_tcb->tid); */
    return current_tcb;
}

struct pcb *sched_current_proc() {
    if (!current_tcb || !current_tcb->parent) {
        print("Couldn't get the current PCB\n");
        return NULL;
    }

    return current_tcb->parent;
}

struct tcb *block_current() {
    asm volatile ("cli");
    current_tcb->state = Blocked;

    struct tcb *t = current_tcb;
    schedule();

    while (current_tcb == t && t->state == Blocked) {
        asm volatile ("sti");
        asm volatile ("hlt");
        asm volatile ("cli");
    }
    return t;
}

void sched_sleep_thread(struct tcb *t) {
    t->state = Sleeping;
}

void sched_wake_thread(struct tcb *t) {
    t->state = Ready;
}

void unblock(struct tcb *t) {
    uint64_t flags;
    asm volatile ("pushfq; pop %0" : "=r"(flags));
    asm volatile ("cli");
    t->state = Ready;
    asm volatile ("push %0; popfq" :: "r"(flags));
}

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
    current_tcb->timed = 0;
    return t;
}

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