#include <stdint.h>
#include <stddef.h>
#include <init.h>

#define MAX_FUNCTIONS 256

struct init_entry {
    init_func_t func;
    uint64_t arg1;
    uint64_t arg2;
};

static struct init_entry init_table[MAX_FUNCTIONS];
static uint64_t init_count = 0;

void register_function(init_func_t func, uint64_t arg1, uint64_t arg2) {
    if (init_count >= MAX_FUNCTIONS || func == NULL) {
        return;
    }

    init_table[init_count].func = func;
    init_table[init_count].arg1 = arg1;
    init_table[init_count].arg2 = arg2;
    init_count++;
}

void init() {
    for (uint64_t i = 0; i < init_count; i++) {
        init_table[i].func(init_table[i].arg1, init_table[i].arg2);
    }
}
