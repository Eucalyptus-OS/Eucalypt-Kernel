#pragma once

#define LOG_DEBUG   0
#define LOG_INFO    1
#define LOG_WARN    2
#define LOG_ERROR   3
#define LOG_FATAL   4

#define LOG_LEVEL LOG_DEBUG

#include <stdarg.h>

void printk_init();
void printk(const char *fmt, ...);
void printk_level(int level, const char *fmt, ...);
void vprintk(const char *fmt, va_list ap);
void vprintk_level(int level, const char *fmt, va_list ap);

#define log_debug(fmt, ...) printk_level(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)  printk_level(LOG_INFO,  fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  printk_level(LOG_WARN,  fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) printk_level(LOG_ERROR, fmt, ##__VA_ARGS__)
#define log_fatal(fmt, ...) printk_level(LOG_FATAL, fmt, ##__VA_ARGS__)