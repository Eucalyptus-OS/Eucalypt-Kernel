#pragma once

void print_impl(const char *file, const char *caller, int line, const char *fmt, ...);

#define print(fmt, ...) print_impl(__FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)
