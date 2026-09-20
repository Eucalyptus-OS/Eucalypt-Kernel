#include <stdint.h>
#include <stdarg.h>
#include <portio.h>
#include <sync/spinlock.h>
#include <logging/format.h>
#include <logging/print.h>

// QEMU/Bochs debug port; on console via -debugcon, else written to the log.
#define DEBUG_PORT 0xE9

// Serializes console output so lines never interleave across CPUs.
static spinlock_t print_lock = 0;

// Emit one character to the debug port, expanding LF into CRLF.
static void putchar(char c) {
    if (c == '\n')
        outb(DEBUG_PORT, '\r');
    outb(DEBUG_PORT, c);
}

// Write a NUL-terminated string to the debug port.
static void print_string(const char *s) {
    while (*s) {
        putchar(*s++);
    }
}

// Write an unsigned 64-bit integer to the debug port in decimal.
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

// Print "file:line: caller(): message" atomically to the debug port.
void print_impl(const char *file, const char *caller, int line, const char *fmt, ...) {
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
}
