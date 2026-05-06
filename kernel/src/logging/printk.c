#include <stdarg.h>
#include <limine.h>
#include <logging/flanterm.h>
#include <logging/flanterm_backends/fb.h>
#include <logging/format.h>
#include <logging/printk.h>

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

__attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

static struct flanterm_context *ft_ctx;

void printk_init() {
    struct limine_framebuffer_response *fb = framebuffer_request.response;
    if (!fb || fb->framebuffer_count == 0) return;

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

static void print_char(char c) {
    flanterm_write(ft_ctx, &c, 1);
}

static void print_str(const char *s) {
    while (*s) print_char(*s++);
}

void printk(const char *fmt, ...) {
    va_list list;
    va_start(list, fmt);
    print_char('\r');
    format(print_char, fmt, list);
    va_end(list);
}

void printk_level(int level, const char *fmt, ...) {
    if (level < LOG_LEVEL) return;
    if (level < LOG_DEBUG || level > LOG_FATAL) level = LOG_DEBUG;

    print_char('\r');
    print_str(level_color[level]);
    print_str(level_prefix[level]);
    print_str(ANSI_RESET);

    va_list list;
    va_start(list, fmt);
    format(print_char, fmt, list);
    va_end(list);

    print_char('\n');
}

void vprintk(const char *fmt, va_list ap) {
    print_char('\r');
    format(print_char, fmt, ap);
}

static char fmt_buf[1024];
static size_t fmt_buf_pos;

static void buf_write_char(char c) {
    if (fmt_buf_pos < sizeof(fmt_buf) - 1)
        fmt_buf[fmt_buf_pos++] = c;
}

void vprintk_level(int level, const char *fmt, va_list ap) {
    if (level < LOG_LEVEL) return;
    if (level < LOG_DEBUG || level > LOG_FATAL) level = LOG_DEBUG;

    fmt_buf_pos = 0;
    format(buf_write_char, fmt, ap);
    fmt_buf[fmt_buf_pos] = '\0';

    print_char('\r');
    print_str(level_color[level]);
    print_str(level_prefix[level]);
    print_str(ANSI_RESET);

    for (size_t i = 0; i < fmt_buf_pos; i++) {
        if (fmt_buf[i] == '\n' && fmt_buf[i + 1] != '\0') {
            print_char('\n');
            print_char('\r');
            print_str(level_color[level]);
            print_str(level_prefix[level]);
            print_str(ANSI_RESET);
        } else {
            print_char(fmt_buf[i]);
        }
    }
    print_char('\n');
}

#define log_debug(fmt, ...) printk_level(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)  printk_level(LOG_INFO,  fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  printk_level(LOG_WARN,  fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) printk_level(LOG_ERROR, fmt, ##__VA_ARGS__)
#define log_fatal(fmt, ...) printk_level(LOG_FATAL, fmt, ##__VA_ARGS__)