#include <stdint.h>
#include <flanterm/flanterm.h>
#include <x86_64/allocator/heap.h>
#include <x86_64/interrupts/timer.h>
#include <ramdisk/fat12.h>
#include <ramdisk/ramdisk.h>
#include <elf.h>

extern struct flanterm_context *ft_ctx;

#define SYSCALL_NULL            0
#define SYSCALL_WRITE           1
#define SYSCALL_KMALLOC         2
#define SYSCALL_KFREE           3
#define SYSCALL_WRITE_FILE      4
#define SYSCALL_READ_FILE       5
#define SYSCALL_EXEC            6
#define SYSCALL_LS              7
#define SYSCALL_SLEEP           8

#define ERR_INVALID_ARG         -1
#define ERR_FILE_NOT_FOUND      -2
#define ERR_READ_FAILED         -3

typedef struct {
    char name[13];
    uint32_t size;
    uint8_t attr;
} file_info_t;

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
    kfree((void *)ptr);
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

static int64_t handle_ls(uint64_t user_buffer_ptr, uint64_t max_entries) {
    if (!user_buffer_ptr || max_entries == 0) {
        return ERR_INVALID_ARG;
    }
    
    file_info_t *user_buffer = (file_info_t *)user_buffer_ptr;
    
    uint8_t *buffer = kmalloc(512);
    if (!buffer) {
        return ERR_INVALID_ARG;
    }
    
    uint32_t file_count = 0;
    
    for (uint32_t sector = 0; sector < 14 && file_count < max_entries; sector++) {
        read_ramdisk_sector(19 + sector, buffer);
        
        for (int i = 0; i < 16 && file_count < max_entries; i++) {
            dir_entry_t *entry = (dir_entry_t *)(buffer + i * 32);
            
            if (entry->name[0] == 0x00) {
                goto done;
            }
            
            if (entry->name[0] == 0xE5) {
                continue;
            }
            
            if (entry->attr & 0x08) {
                continue;
            }
            
            int pos = 0;
            for (int j = 0; j < 8 && entry->name[j] != ' '; j++) {
                user_buffer[file_count].name[pos++] = entry->name[j];
            }
            
            if (entry->ext[0] != ' ') {
                user_buffer[file_count].name[pos++] = '.';
                for (int j = 0; j < 3 && entry->ext[j] != ' '; j++) {
                    user_buffer[file_count].name[pos++] = entry->ext[j];
                }
            }
            
            user_buffer[file_count].name[pos] = '\0';
            user_buffer[file_count].size = entry->file_size;
            user_buffer[file_count].attr = entry->attr;
            
            file_count++;
        }
    }
    
done:
    kfree(buffer);
    return file_count;
}

static int64_t handle_sleep(uint64_t time_ms) {
    if (time_ms == 0) {
        return 0;
    }

    timer_wait_ms(time_ms);
    return 0;
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
            
        case SYSCALL_LS:
            return handle_ls(arg1, arg2);
            
        case SYSCALL_SLEEP:
            return handle_sleep(arg1);
            
        default:
            return ERR_INVALID_ARG;
    }
}