#include <stdint.h>
#include <mm/heap.h>

struct tcb {
    uint16_t tid;
    uintptr_t stack_top;
    void *entry;
};

uint16_t next_tid = 0;

uint8_t create_thread(void *entry, uint16_t stack_size) {
    void *ptr = kmalloc(stack_size);
    if (ptr == NULL) {
        return 1;
    }
    uintptr_t stack_top = (uintptr_t)ptr + stack_size;

    struct tcb tcb = {
        .tid = next_tid++,
        .stack_top = stack_top,
        .entry = entry,
    };

    return 0;
}