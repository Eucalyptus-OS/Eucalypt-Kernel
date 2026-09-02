#pragma once

#include <stdint.h>

typedef uint8_t (*init_func_t)(uint64_t, uint64_t);

void register_function_impl(init_func_t func, const char *name, uint64_t arg1, uint64_t arg2);
void init();

#define register_function(func, arg1, arg2) \
    register_function_impl((init_func_t)(void *)(func), #func, (arg1), (arg2))
void init();
