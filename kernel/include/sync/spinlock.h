#pragma once

#include <stdint.h>

typedef volatile int spinlock_t;

static inline void spinlock_acquire(spinlock_t *lock) {
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(lock, __ATOMIC_RELAXED))
            __asm__ volatile ("pause");
}

static inline void spinlock_release(spinlock_t *lock) {
    __atomic_clear(lock, __ATOMIC_RELEASE);
}

static inline uint64_t save_irq() {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile ("cli");
    return flags;
}

static inline void restore_irq(uint64_t flags) {
    if (flags & (1ULL << 9))
        __asm__ volatile ("sti");
}

static inline uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uint64_t flags = save_irq();
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(lock, __ATOMIC_RELAXED))
            __asm__ volatile ("pause");
    return flags;
}

static inline void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
    __atomic_clear(lock, __ATOMIC_RELEASE);
    restore_irq(flags);
}