#include <stddef.h>
#include <stdbool.h>
#include <smp.h>
#include <logging/printk.h>
#include <gdt/gdt.h>
#include <mm/heap.h>
#include <multitasking/thread.h>
#include <multitasking/proc.h>
#include <multitasking/sched.h>

extern void context_switch(struct tcb *current, struct tcb *next);

typedef struct {
    struct tcb *threads[MAX_THREADS];
    int         front;
    int         rear;
    int         count;
} threads_t;

static threads_t ready_queue_data;
struct tcb *current_thread = NULL;
threads_t  *tq = &ready_queue_data;
static bool enabled = false;

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
    if (t == current_thread) sched_yield();
}

void sched_wake(struct tcb *t) {
    if (t->state != blocked) {
        return;
    }
    t->state = ready;
    enqueue(t);
}

uintptr_t schedule(uintptr_t rsp) {
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE)) return rsp;

    if (current_thread == NULL) {
        current_thread = dequeue();
        if (!current_thread) {
            log_debug("schedule: queue empty, nothing to run\n");
            return rsp;
        }
        current_thread->state = running;
        tss.rsp0 = kernel_stack_top(current_thread);
        
        __asm__ volatile("mov %0, %%cr3" :: "r"(current_thread->cr3));
        return current_thread->rsp;
    }

    if (rsp != 0) current_thread->rsp = rsp;

    if (current_thread->state == dead) {
        log_debug("schedule: freeing dead tid=%d\n", current_thread->tid);
        if (current_thread->ustack_base) kfree(current_thread->ustack_base);
        if (current_thread->stack_base)  kfree(current_thread->stack_base);
        kfree(current_thread);
        current_thread = NULL;
    } else if (current_thread->state != blocked) {
        current_thread->state = ready;
        enqueue(current_thread);
    } else {
        log_debug("schedule: tid=%d staying blocked, not requeued\n", current_thread->tid);
    }

    if (tq->count == 0) {
        log_debug("schedule: queue empty after preempt, halting\n");
        current_thread = NULL;
        __asm__ volatile("sti; hlt");
        return rsp;
    }

    current_thread = dequeue();
    current_thread->state = running;
    tss.rsp0 = kernel_stack_top(current_thread);

    paddr current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_thread->cr3 != current_cr3)
        __asm__ volatile("mov %0, %%cr3" :: "r"(current_thread->cr3));

    return current_thread->rsp;
}

int32_t get_current_pid(void) {
    if (!current_thread || !current_thread->parent) return -1;
    return current_thread->parent->pid;
}

int32_t get_current_ppid(void) {
    if (!current_thread || !current_thread->parent) return -1;
    struct pcb *proc = proc_get(current_thread->parent->pid);
    if (!proc) return -1;
    return proc->parent_pid;
}

struct tcb *get_current_thread(void) {
    return current_thread;
}

struct tcb *get_thread_copy(uint16_t tid) {
    for (int i = 0; i < tq->count; i++) {
        int idx = (tq->front + i) % MAX_THREADS;
        if (tq->threads[idx]->tid == tid)
            return tq->threads[idx];
    }
    return NULL;
}

struct tcb **get_thread(uint16_t tid) {
    for (int i = 0; i < tq->count; i++) {
        int idx = (tq->front + i) % MAX_THREADS;
        if (tq->threads[idx]->tid == tid)
            return &tq->threads[idx];
    }
    return NULL;
}
