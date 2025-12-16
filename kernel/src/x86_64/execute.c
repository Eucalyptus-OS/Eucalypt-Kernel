#include <x86_64/execute.h>
#include <x86_64/allocator/heap.h>
#include <stdint.h>

#define EXEC_MAGIC 0x004E4942 

uint64_t kernel_stack_save = 0;

int load_and_execute_app(void *app_data, size_t app_size) {
    if (app_size < 4) {
        return -1;
    }
    
    uint32_t *magic = (uint32_t *)app_data;
    if (*magic != EXEC_MAGIC) {
        return -2;
    }
    
    void *app_entry = (void *)((uint8_t *)app_data + 4);
    size_t code_size = app_size - 4;
    
    if (code_size == 0) {
        return -3;
    }
    
    uint8_t *stack = kmalloc(4096);
    if (!stack) {
        return -4;
    }
    
    uint64_t stack_top = ((uint64_t)stack + 4096) & ~0xFULL;
    
    __asm__ volatile (
        "mov %%rsp, %0\n\t"
        "mov %1, %%rsp\n\t"
        "call *%2\n\t"
        "mov %0, %%rsp\n\t"  
        : "=m"(kernel_stack_save)
        : "r"(stack_top), "r"(app_entry)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory"
    );
    
    kfree(stack);
    return 0;
}