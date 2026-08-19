#pragma once

#include <stdint.h>

typedef void (*init_func_t)(uint64_t, uint64_t);

void register_function(init_func_t func, uint64_t arg1, uint64_t arg2);
void init();
