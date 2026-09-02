#pragma once

#include <stdint.h>

extern volatile uint32_t preempt_depth;

void schedule();
struct tcb *get_current_thread();
struct tcb *block_current();
void sched_sleep_thread(struct tcb *t);
void sched_wake_thread(struct tcb *t);
void unblock(struct tcb *t);

static inline void preempt_disable(void) {
    __atomic_add_fetch(&preempt_depth, 1, __ATOMIC_RELAXED);
}

static inline void preempt_enable(void) {
    __atomic_sub_fetch(&preempt_depth, 1, __ATOMIC_RELAXED);
}
