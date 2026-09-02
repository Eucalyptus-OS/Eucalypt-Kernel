#include <stdint.h>
#include <stdarg.h>
#include <portio.h>
#include <sync/spinlock.h>
#include <multitasking/sched.h>
#include <logging/format.h>
#include <logging/print.h>

// Debugcon port: QEMU prints anything written here to the -debugcon device.
#define DEBUG_PORT 0xE9

static spinlock_t print_lock = 0;

static void putchar(char c) {
    if (c == '\n')
        outb(DEBUG_PORT, '\r');
    outb(DEBUG_PORT, c);
}

static void print_string(const char *s) {
    while (*s) {
        putchar(*s++);
    }
}

static void print_uint(uint64_t n) {
    char buf[20];
    int i = 0;
    do {
        buf[i++] = '0' + (char)(n % 10);
        n /= 10;
    } while (n);
    while (i > 0) {
        putchar(buf[--i]);
    }
}

void print_impl(const char *file, const char *caller, int line, const char *fmt, ...) {
    preempt_disable();

    // irqsave so an interrupt during a print can't deadlock or interleave output
    uint64_t flags = spinlock_acquire_irqsave(&print_lock);

    print_string(file);
    putchar(':');
    print_uint(line);
    print_string(": ");
    print_string(caller);
    print_string("(): ");

    va_list list;
    va_start(list, fmt);
    format(putchar, fmt, list);
    va_end(list);

    spinlock_release_irqrestore(&print_lock, flags);

    preempt_enable();
}
