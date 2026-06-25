#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <smp.h>
#include <interrupts/apic.h>
#include <sync/spinlock.h>
#include <logging/printk.h>
#include <gdt/gdt.h>
#include <mm/heap.h>
#include <multitasking/thread.h>
#include <multitasking/proc.h>
#include <multitasking/sched.h>

#define MAX_CPUS 100

typedef struct {
    struct tcb *threads[MAX_THREADS];
    int         front;
    int         rear;
    int         count;
} threads_t;

typedef struct cpu_sched {
    struct tcb *current;
    threads_t   run_queue;
    spinlock_t  lock;
} cpu_sched_t;

extern void context_switch(struct tcb *current, struct tcb *next);

static threads_t  ready_queue_data;
static bool       enabled = false;
threads_t *tq = &ready_queue_data;
struct cpu_sched   sched_cpus[MAX_CPUS];

static uint8_t cpu_to_schedule = 1;
static spinlock_t sched_lock;

static uintptr_t kernel_stack_top(struct tcb *thread) {
    uintptr_t aligned = ((uintptr_t)thread->stack_base + 0xFFF) & ~0xFFFULL;
    return aligned + KERNEL_STACK_SIZE;
}

void enable_sched(void) {
    __atomic_store_n(&enabled, true, __ATOMIC_RELEASE);
}

void disable_sched(void) {
    __atomic_store_n(&enabled, false, __ATOMIC_RELEASE);
}

void scheduler_init(void) {
    tq->front = 0;
    tq->rear  = 0;
    tq->count = 0;
    disable_sched();
}

bool enqueue(struct tcb *thread) {
    if (tq->count == MAX_THREADS) {
        log_error("sched: enqueue FAILED, queue full (count=%d) tid=%d\n",
                  tq->count, thread ? thread->tid : -1);
        return false;
    }

    tq->threads[tq->rear] = thread;
    tq->rear = (tq->rear + 1) % MAX_THREADS;
    tq->count++;

    return true;
}

struct tcb *dequeue(void) {
    if (tq->count == 0) {
        return NULL;
    }

    struct tcb *thread = tq->threads[tq->front];
    tq->front = (tq->front + 1) % MAX_THREADS;
    tq->count--;

    return thread;
}

void sched_yield(void) {
    __asm__ volatile("int $0x20");
}

void sched_sleep(struct tcb *t) {
    t->state = blocked;
    if (t == get_current_thread()) sched_yield();
}

void sched_wake(struct tcb *t) {
    if (t->state != blocked) {
        return;
    }

    t->state = ready;
    enqueue(t);
}

uintptr_t schedule(uintptr_t rsp) {
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE)) {
        return rsp;
    }

    uint32_t id = apic_id();

    spinlock_acquire(&sched_lock);

    struct tcb *prev = sched_cpus[id].current;

    if (prev == NULL) {
        struct tcb *next = dequeue();
        if (!next) {
            spinlock_release(&sched_lock);
            return rsp;
        }
        next->state = running;
        sched_cpus[id].current = next;
        tss.rsp0 = kernel_stack_top(next);
        spinlock_release(&sched_lock);

        __asm__ volatile("mov %0, %%cr3" :: "r"(next->cr3));
        return next->rsp;
    }

    if (rsp != 0) {
        prev->rsp = rsp;
    }

    if (prev->state == dead) {
        spinlock_release(&sched_lock);
        if (prev->ustack_base) kfree(prev->ustack_base);
        if (prev->stack_base)  kfree(prev->stack_base);
        kfree(prev);
        spinlock_acquire(&sched_lock);
        sched_cpus[id].current = NULL;
    } else if (prev->state != blocked) {
        prev->state = ready;
        enqueue(prev);
    }

    if (tq->count == 0) {
        sched_cpus[id].current = NULL;
        spinlock_release(&sched_lock);
        __asm__ volatile("sti\nhlt");
        return rsp;
    }

    struct tcb *next = dequeue();
    next->state = running;
    sched_cpus[id].current = next;
    tss.rsp0 = kernel_stack_top(next);

    spinlock_release(&sched_lock);

    paddr current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if (next->cr3 != current_cr3) {
        __asm__ volatile("mov %0, %%cr3" :: "r"(next->cr3));
    }
    return next->rsp;
}

int32_t get_current_pid(void) {
    struct tcb *t = get_current_thread();
    if (!t || !t->parent) return -1;
    return t->parent->pid;
}

int32_t get_current_ppid(void) {
    struct tcb *t = get_current_thread();
    if (!t || !t->parent) return -1;

    struct pcb *proc = proc_get(t->parent->pid);
    if (!proc) return -1;

    return proc->parent_pid;
}

struct tcb *get_current_thread(void) {
    return sched_cpus[apic_id()].current;
}

struct tcb *get_thread_copy(uint16_t tid) {
    for (int i = 0; i < tq->count; i++) {
        int idx = (tq->front + i) % MAX_THREADS;
        if (tq->threads[idx]->tid == tid) {
            return tq->threads[idx];
        }
    }
    return NULL;
}

struct tcb **get_thread(uint16_t tid) {
    for (int i = 0; i < tq->count; i++) {
        int idx = (tq->front + i) % MAX_THREADS;
        if (tq->threads[idx]->tid == tid) {
            return &tq->threads[idx];
        }
    }
    return NULL;
}