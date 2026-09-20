#pragma once

// Low-level backend; callers should use the print() macro below.
void print_impl(const char *file, const char *caller, int line, const char *fmt, ...);
// printf-style logging that injects file, function and line automatically.
#define print(fmt, ...) print_impl(__FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)
