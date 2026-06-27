#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <logging/flanterm.h>
#include <logging/flanterm_backends/fb.h>
#include <logging/format.h>
#include <logging/printk.h>
#include <sync/spinlock.h>

static const char *level_prefix[] = {
    "[DEBUG] ",
    "[INFO]  ",
    "[WARN]  ",
    "[ERROR] ",
    "[FATAL] "
};

static const char *level_color[] = {
    "\033[36m",
    "\033[32m",
    "\033[33m",
    "\033[31m",
    "\033[35m",
};

#define ANSI_RESET "\033[0m"
#define PRINTK_BUFFER_SIZE 65536
#define PRINTK_FORMAT_SIZE 1024
#define PRINTK_DRAIN_BUDGET 512

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

struct flanterm_context *ft_ctx;
static spinlock_t printk_lock = 0;
static bool printk_framebuffer_enabled = true;
static char printk_buffer[PRINTK_BUFFER_SIZE];
static size_t printk_head = 0;
static size_t printk_tail = 0;
static size_t printk_count = 0;
static size_t printk_dropped = 0;

static char fmt_buf[PRINTK_FORMAT_SIZE];
static size_t fmt_buf_pos;

void printk_init(void) {
    struct limine_framebuffer_response *fb = framebuffer_request.response;
    if (!fb || fb->framebuffer_count == 0) {
        return;
    }
    ft_ctx = flanterm_fb_init(
        NULL,
        NULL,
        fb->framebuffers[0]->address, fb->framebuffers[0]->width,
        fb->framebuffers[0]->height, fb->framebuffers[0]->pitch,
        fb->framebuffers[0]->red_mask_size, fb->framebuffers[0]->red_mask_shift,
        fb->framebuffers[0]->green_mask_size, fb->framebuffers[0]->green_mask_shift,
        fb->framebuffers[0]->blue_mask_size, fb->framebuffers[0]->blue_mask_shift,
        NULL,
        NULL, NULL,
        NULL, NULL,
        NULL, NULL,
        NULL, 0, 0, 1,
        0, 0,
        0,
        0
    );
}

static bool irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return (flags & (1ULL << 9)) != 0;
}

static void irq_restore(bool enabled) {
    if (enabled) {
        __asm__ volatile ("sti" ::: "memory");
    }
}

void printk_set_framebuffer_enabled(bool enabled) {
    bool had_irqs = irq_save();
    spinlock_acquire(&printk_lock);

    printk_framebuffer_enabled = enabled;
    if (!enabled) {
        printk_head = 0;
        printk_tail = 0;
        printk_count = 0;
    }

    spinlock_release(&printk_lock);
    irq_restore(had_irqs);
}

static void print_char(char c) {
    if (!ft_ctx || !printk_framebuffer_enabled) {
        return;
    }
    flanterm_write(ft_ctx, &c, 1);
}

static void printk_queue_char_locked(char c) {
    if (printk_count == sizeof(printk_buffer)) {
        printk_tail = (printk_tail + 1) % sizeof(printk_buffer);
        printk_count--;
        printk_dropped++;
    }

    printk_buffer[printk_head] = c;
    printk_head = (printk_head + 1) % sizeof(printk_buffer);
    printk_count++;
}

static void printk_queue_str_locked(const char *s) {
    while (*s) {
        printk_queue_char_locked(*s++);
    }
}

static void buf_write_char(char c) {
    if (fmt_buf_pos < sizeof(fmt_buf) - 1) {
        fmt_buf[fmt_buf_pos++] = c;
    }
}

static void printk_queue_formatted_locked(const char *fmt, va_list ap) {
    fmt_buf_pos = 0;
    format(buf_write_char, fmt, ap);
    fmt_buf[fmt_buf_pos] = '\0';
}

static void printk_queue_level_locked(int level) {
    printk_queue_char_locked('\r');
    printk_queue_str_locked(level_color[level]);
    printk_queue_str_locked(level_prefix[level]);
    printk_queue_str_locked(ANSI_RESET);
}

static void printk_queue_level_message_locked(int level, const char *fmt, va_list ap) {
    printk_queue_formatted_locked(fmt, ap);
    printk_queue_level_locked(level);
    for (size_t i = 0; i < fmt_buf_pos; i++) {
        printk_queue_char_locked(fmt_buf[i]);
        if (fmt_buf[i] == '\n' && fmt_buf[i + 1] != '\0') {
            printk_queue_level_locked(level);
        }
    }
}

static void printk_drain_locked(size_t budget) {
    while (printk_count > 0 && budget-- > 0) {
        char c = printk_buffer[printk_tail];
        printk_tail = (printk_tail + 1) % sizeof(printk_buffer);
        printk_count--;
        print_char(c);
    }
}

static void printk_drain_all_locked(void) {
    while (printk_count > 0) {
        printk_drain_locked(PRINTK_DRAIN_BUDGET);
    }
}

void printk(const char *fmt, ...) {
    bool had_irqs = irq_save();
    spinlock_acquire(&printk_lock);

    va_list list;
    va_start(list, fmt);
    printk_queue_formatted_locked(fmt, list);
    va_end(list);
    for (size_t i = 0; i < fmt_buf_pos; i++) {
        printk_queue_char_locked(fmt_buf[i]);
    }
    printk_queue_char_locked('\r');
    if (!had_irqs) {
        printk_drain_all_locked();
    }

    spinlock_release(&printk_lock);
    irq_restore(had_irqs);
}

void printk_level(int level, const char *fmt, ...) {
    if (level < LOG_LEVEL) {
        return;
    }
    if (level < LOG_DEBUG || level > LOG_FATAL) {
        level = LOG_DEBUG;
    }

    bool had_irqs = irq_save();
    spinlock_acquire(&printk_lock);

    va_list list;
    va_start(list, fmt);
    printk_queue_level_message_locked(level, fmt, list);
    va_end(list);
    if (!had_irqs || level == LOG_FATAL) {
        printk_drain_all_locked();
    }

    spinlock_release(&printk_lock);
    irq_restore(had_irqs);
}

void vprintk(const char *fmt, va_list ap) {
    bool had_irqs = irq_save();
    spinlock_acquire(&printk_lock);

    printk_queue_formatted_locked(fmt, ap);
    for (size_t i = 0; i < fmt_buf_pos; i++) {
        printk_queue_char_locked(fmt_buf[i]);
    }
    printk_queue_char_locked('\r');
    if (!had_irqs) {
        printk_drain_all_locked();
    }

    spinlock_release(&printk_lock);
    irq_restore(had_irqs);
}

void vprintk_level(int level, const char *fmt, va_list ap) {
    if (level < LOG_LEVEL) {
        return;
    }
    if (level < LOG_DEBUG || level > LOG_FATAL) {
        level = LOG_DEBUG;
    }

    bool had_irqs = irq_save();
    spinlock_acquire(&printk_lock);

    printk_queue_level_message_locked(level, fmt, ap);
    if (!had_irqs || level == LOG_FATAL) {
        printk_drain_all_locked();
    }

    spinlock_release(&printk_lock);
    irq_restore(had_irqs);
}

void printk_drain_tick(void) {
    bool had_irqs = irq_save();
    spinlock_acquire(&printk_lock);
    printk_drain_locked(PRINTK_DRAIN_BUDGET);
    spinlock_release(&printk_lock);
    irq_restore(had_irqs);
}
