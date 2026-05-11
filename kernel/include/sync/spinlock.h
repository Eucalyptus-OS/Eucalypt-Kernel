#pragma once

typedef volatile int spinlock_t;

static inline void spinlock_acquire(spinlock_t *lock) {
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(lock, __ATOMIC_RELAXED))
            __asm__ volatile ("pause");
}

static inline void spinlock_release(spinlock_t *lock) {
    __atomic_clear(lock, __ATOMIC_RELEASE);
}