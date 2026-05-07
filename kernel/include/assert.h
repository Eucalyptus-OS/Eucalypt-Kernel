#pragma once

#include <stddef.h>

#ifndef likely
#  define likely(x)   __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#  define unlikely(x) __builtin_expect(!!(x), 0)
#endif

__attribute__((noreturn)) void panic(const char *fmt, ...);

#ifndef NDEBUG
#  define ASSERT(expr)                                                        \
    do {                                                                      \
        if (unlikely(!(expr))) {                                              \
            panic("Assertion failed: (%s)\n"                                  \
                  "  file : %s:%d\n"                                          \
                  "  func : %s\n",                                            \
                  #expr, __FILE__, __LINE__, __func__);                       \
        }                                                                     \
    } while (0)
#else
#  define ASSERT(expr) ((void)(expr))
#endif

#ifndef NDEBUG
#  define ASSERT_MSG(expr, msg)                                               \
    do {                                                                      \
        if (unlikely(!(expr))) {                                              \
            panic("Assertion failed: (%s) — %s\n"                             \
                         "  file : %s:%d\n"                                   \
                         "  func : %s\n",                                     \
                         #expr, (msg), __FILE__, __LINE__, __func__);         \
        }                                                                     \
    } while (0)
#else
#  define ASSERT_MSG(expr, msg) ((void)(expr))
#endif

#define PANIC(msg)                                                            \
    panic("Panic: %s\n"                                                       \
          "  file : %s:%d\n"                                                  \
          "  func : %s\n",                                                    \
          (msg), __FILE__, __LINE__, __func__)

#ifndef NDEBUG
#  define UNREACHABLE()  PANIC("Reached unreachable code")
#else
#  define UNREACHABLE()  __builtin_unreachable()
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define STATIC_ASSERT(expr, msg)  _Static_assert(expr, msg)
#else
#  define STATIC_ASSERT(expr, msg) \
    typedef char _static_assert_##__LINE__[(expr) ? 1 : -1] \
        __attribute__((unused))
#endif

#define ASSERT_NOT_NULL(ptr)   ASSERT((ptr) != NULL)

#define ASSERT_EQ(a, b)        ASSERT((a) == (b))

#define ASSERT_NE(a, b)        ASSERT((a) != (b))

#define ASSERT_RANGE(val, lo, hi) ASSERT((val) >= (lo) && (val) < (hi))

#define ASSERT_ALIGNED(addr, align) ASSERT(((uintptr_t)(addr) & ((align) - 1)) == 0)