#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include <smp.h>
#include <interrupts/apic.h>
#include <sync/spinlock.h>
#include <logging/printk.h>
#include <gdt/gdt.h>
#include <mm/heap.h>
#include <mm/hhdm.h>
#include <mm/paging.h>
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
    struct tcb *reap;
    threads_t   run_queue;
    spinlock_t  lock;
} cpu_sched_t;

static threads_t  ready_queue_data;
static bool       enabled = false;
threads_t *tq = &ready_queue_data;
struct cpu_sched   sched_cpus[MAX_CPUS];

static spinlock_t sched_lock;
static struct tcb  idle_tcbs[MAX_CPUS];
static bool        idle_ready[MAX_CPUS];

static uintptr_t kernel_stack_top(struct tcb *thread) {
    uintptr_t aligned = ((uintptr_t)thread->stack_base + 0xFFF) & ~0xFFFULL;
    return aligned + KERNEL_STACK_SIZE;
}

static uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

static void lock_sched(uint64_t *flags) {
    *flags = irq_save();
    spinlock_acquire(&sched_lock);
}

static void unlock_sched(uint64_t flags) {
    spinlock_release(&sched_lock);
    irq_restore(flags);
}

static uint64_t setup_idle_stack(uint8_t *stack_base, uint64_t stack_size, void *entry) {
    uint64_t *stack_top = (uint64_t *)(stack_base + stack_size);
    uint64_t *rsp       = stack_top;

    *--rsp = 0x10;
    *--rsp = (uint64_t)stack_top;
    *--rsp = 0x202;
    *--rsp = 0x08;
    *--rsp = (uint64_t)entry;

    for (int i = 0; i < 15; i++) {
        *--rsp = 0;
    }

    return (uint64_t)rsp;
}

static void idle_entry(void) {
    for (;;) {
        __asm__ volatile("sti\nhlt" ::: "memory");
    }
}

static bool is_idle_thread(struct tcb *thread) {
    return thread &&
           thread->owning_cpu < MAX_CPUS &&
           idle_ready[thread->owning_cpu] &&
           thread == &idle_tcbs[thread->owning_cpu];
}

static void update_rsp0(uint8_t id, struct tcb *thread) {
    uintptr_t rsp0 = kernel_stack_top(thread);
    if (id < MAX_CPUS && per_cpu_data[id]) {
        per_cpu_data[id]->tss.rsp0 = rsp0;
    } else {
        tss.rsp0 = rsp0;
    }
}

static bool enqueue_locked(struct tcb *thread) {
    if (!thread || is_idle_thread(thread)) {
        return false;
    }

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

static struct tcb *dequeue_locked(void) {
    if (tq->count == 0) {
        return NULL;
    }

    struct tcb *thread = tq->threads[tq->front];
    tq->threads[tq->front] = NULL;
    tq->front = (tq->front + 1) % MAX_THREADS;
    tq->count--;

    return thread;
}

static void reap_dead_thread(struct tcb *thread) {
    if (!thread || is_idle_thread(thread)) {
        return;
    }

    if (thread->ustack_base) {
        kfree(thread->ustack_base);
    }
    if (thread->stack_base) {
        kfree(thread->stack_base);
    }
    kfree(thread);
}

static void reap_cpu_deferred(uint8_t id) {
    if (id >= MAX_CPUS) {
        return;
    }

    struct tcb *thread = sched_cpus[id].reap;
    if (!thread) {
        return;
    }

    sched_cpus[id].reap = NULL;
    reap_dead_thread(thread);
}

static void init_idle_thread(uint8_t id) {
    if (id >= MAX_CPUS || idle_ready[id]) {
        return;
    }

    void *raw = kmalloc(KERNEL_STACK_SIZE + 0x1000);
    if (!raw) {
        return;
    }

    uintptr_t aligned = ((uintptr_t)raw + 0xFFF) & ~0xFFFULL;
    struct tcb *idle = &idle_tcbs[id];

    idle->tid         = UINT16_MAX;
    idle->parent      = NULL;
    idle->cr3         = (paddr)((uintptr_t)kernel_pml4 - offset);
    idle->state       = running;
    idle->stack_base  = raw;
    idle->ustack_base = NULL;
    idle->entry       = idle_entry;
    idle->rsp         = setup_idle_stack((uint8_t *)aligned, KERNEL_STACK_SIZE, idle_entry);
    idle->owning_cpu  = id;

    idle_ready[id] = true;
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
    for (int i = 0; i < MAX_CPUS; i++) {
        sched_cpus[i].current = NULL;
        sched_cpus[i].reap = NULL;
        sched_cpus[i].run_queue.front = 0;
        sched_cpus[i].run_queue.rear = 0;
        sched_cpus[i].run_queue.count = 0;
        sched_cpus[i].lock = 0;
        idle_ready[i] = false;
        init_idle_thread((uint8_t)i);
    }
    disable_sched();
}

bool enqueue(struct tcb *thread) {
    uint64_t flags;
    lock_sched(&flags);
    bool ok = enqueue_locked(thread);
    unlock_sched(flags);
    return ok;
}

struct tcb *dequeue(void) {
    uint64_t flags;
    lock_sched(&flags);
    struct tcb *thread = dequeue_locked();
    unlock_sched(flags);
    return thread;
}

void sched_yield(void) {
    __asm__ volatile("int $0x20");
}

void sched_sleep(struct tcb *t) {
    if (!t || is_idle_thread(t)) {
        return;
    }

    uint64_t flags;
    lock_sched(&flags);
    t->state = blocked;
    uint8_t id = smp_current_cpu_id();
    bool should_yield = (id < MAX_CPUS && t == sched_cpus[id].current);
    unlock_sched(flags);

    if (should_yield) {
        sched_yield();
    }
}

void sched_wake(struct tcb *t) {
    if (!t || is_idle_thread(t)) {
        return;
    }

    uint64_t flags;
    lock_sched(&flags);
    if (t->state != blocked) {
        unlock_sched(flags);
        return;
    }

    t->state = ready;
    enqueue_locked(t);
    unlock_sched(flags);
}

uintptr_t schedule(uintptr_t rsp) {
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE)) {
        return rsp;
    }

    uint8_t id = smp_current_cpu_id();
    if (id >= MAX_CPUS || !idle_ready[id]) {
        return rsp;
    }

    reap_cpu_deferred(id);

    uint64_t flags;
    lock_sched(&flags);

    struct tcb *prev = sched_cpus[id].current;

    if (prev && rsp != 0) {
        prev->rsp = rsp;
    }

    if (prev && !is_idle_thread(prev)) {
        if (prev->state == dead) {
            sched_cpus[id].reap = prev;
            sched_cpus[id].current = NULL;
        } else if (prev->state == blocked) {
            sched_cpus[id].current = NULL;
        } else {
            prev->state = ready;
            enqueue_locked(prev);
        }
    }

    struct tcb *next = dequeue_locked();
    if (!next) {
        next = &idle_tcbs[id];
    }

    next->state = running;
    sched_cpus[id].current = next;
    next->owning_cpu = id;
    update_rsp0(id, next);

    unlock_sched(flags);

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
    uint8_t id = smp_current_cpu_id();
    if (id >= MAX_CPUS) {
        return NULL;
    }
    return sched_cpus[id].current;
}

struct tcb *get_thread_copy(uint16_t tid) {
    uint64_t flags;
    lock_sched(&flags);
    for (int i = 0; i < tq->count; i++) {
        int idx = (tq->front + i) % MAX_THREADS;
        if (tq->threads[idx]->tid == tid) {
            struct tcb *thread = tq->threads[idx];
            unlock_sched(flags);
            return thread;
        }
    }
    unlock_sched(flags);
    return NULL;
}

struct tcb **get_thread(uint16_t tid) {
    uint64_t flags;
    lock_sched(&flags);
    for (int i = 0; i < tq->count; i++) {
        int idx = (tq->front + i) % MAX_THREADS;
        if (tq->threads[idx]->tid == tid) {
            struct tcb **thread = &tq->threads[idx];
            unlock_sched(flags);
            return thread;
        }
    }
    unlock_sched(flags);
    return NULL;
}
