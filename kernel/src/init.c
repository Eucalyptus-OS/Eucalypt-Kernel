#include <init.h>
#include <logging/print.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_FUNCTIONS 256

struct init_entry {
    init_func_t func;
    const char *name;
    uint64_t arg1;
    uint64_t arg2;
};

static struct init_entry init_table[MAX_FUNCTIONS];
static uint64_t init_count = 0;

void register_function_impl(init_func_t func, const char *name, uint64_t arg1, uint64_t arg2) {
    if (init_count >= MAX_FUNCTIONS || func == NULL) {
        return;
    }

    init_table[init_count].func = func;
    init_table[init_count].name = name;
    init_table[init_count].arg1 = arg1;
    init_table[init_count].arg2 = arg2;
    init_count++;
}

void init() {
    uint8_t r = 0;
    for (uint64_t i = 0; i < init_count; i++) {
        r = init_table[i].func(init_table[i].arg1, init_table[i].arg2);
        if (r != 0) {
            print("'%s'(0x%lx, 0x%lx) failed with error %d\n",
                  init_table[i].name, init_table[i].arg1, init_table[i].arg2, r);
            asm volatile ("cli\nhlt");
        }
    }
}
