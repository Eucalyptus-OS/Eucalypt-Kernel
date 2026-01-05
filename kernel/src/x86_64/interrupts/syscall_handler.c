// syscall.c
#include <stdint.h>
#include <flanterm/flanterm.h>
#include <x86_64/allocator/heap.h>
#include <ramdisk/fat12.h>
#include <elf.h>

extern struct flanterm_context *ft_ctx;

#define SYSCALL_NULL            0
#define SYSCALL_WRITE           1
#define SYSCALL_KMALLOC         2
#define SYSCALL_KFREE           3
#define SYSCALL_WRITE_FILE      4
#define SYSCALL_READ_FILE       5
#define SYSCALL_EXEC            6

#define ERR_INVALID_ARG         -1
#define ERR_FILE_NOT_FOUND      -2
#define ERR_READ_FAILED         -3

static int64_t handle_write(uint64_t str_ptr) {
    if (!str_ptr) {
        return ERR_INVALID_ARG;
    }
    
    flanterm_write(ft_ctx, (const char *)str_ptr);
    return 0;
}

static int64_t handle_kmalloc(uint64_t size) {
    if (size == 0) {
        return 0;
    }
    
    return (int64_t)kmalloc(size);
}

static int64_t handle_kfree(uint64_t ptr) {
    if (!ptr) {
        return ERR_INVALID_ARG;
    }
    
    flanterm_write(ft_ctx, "[SYSCALL] kfree called on: ");
    // print ptr address
    
    flanterm_write(ft_ctx, "[SYSCALL] About to call kfree\n");
    kfree((void *)ptr);
    flanterm_write(ft_ctx, "[SYSCALL] kfree completed\n");
    
    return 0;
}

static int64_t handle_write_file(uint64_t filename_ptr, uint64_t data_ptr) {
    if (!filename_ptr || !data_ptr) {
        return ERR_INVALID_ARG;
    }
    
    write_file((const char *)filename_ptr, (uint8_t *)data_ptr);
    return 0;
}

static int64_t handle_read_file(uint64_t filename_ptr, uint64_t size_out_ptr) {
    if (!filename_ptr || !size_out_ptr) {
        return ERR_INVALID_ARG;
    }
    
    dir_entry_t *file = find_file((const char *)filename_ptr);
    if (!file) {
        return ERR_FILE_NOT_FOUND;
    }
    
    uint32_t size = 0;
    uint8_t *data = read_file(file, &size);
    
    if (!data || size == 0) {
        kfree(file);
        return ERR_READ_FAILED;
    }
    
    *(uint32_t *)size_out_ptr = size;
    kfree(file);
    
    return (int64_t)data;
}

static int64_t handle_exec(uint64_t filename_ptr) {
    return execute_elf((const char *)filename_ptr);
}

int64_t syscall_handler(uint64_t syscall, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    switch (syscall) {
        case SYSCALL_WRITE:
            return handle_write(arg1);
            
        case SYSCALL_KMALLOC:
            return handle_kmalloc(arg1);
            
        case SYSCALL_KFREE:
            return handle_kfree(arg1);
            
        case SYSCALL_WRITE_FILE:
            return handle_write_file(arg1, arg2);
            
        case SYSCALL_READ_FILE:
            return handle_read_file(arg1, arg2);
            
        case SYSCALL_EXEC:
            return handle_exec(arg1);
            
        default:
            return ERR_INVALID_ARG;
    }
}