#pragma once

#include <stdarg.h>
#include <stdint.h>

void smp_console_draw_glyph(uint8_t cpu_id, uint32_t color, char c);
uint8_t smp_console_init(uint8_t cpu_count);
void println(uint8_t cpu_id, const char *str, uint32_t color);
void smp_vprintf(uint8_t cpu_id, uint32_t color, const char *fmt, va_list ap);
void smp_printf(uint8_t cpu_id, uint32_t color, const char *fmt, ...);
