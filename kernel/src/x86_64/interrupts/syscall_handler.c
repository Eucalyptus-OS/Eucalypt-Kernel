#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <limine.h>
#include <flanterm/flanterm.h>
#include <x86_64/allocator/heap.h>
#include <x86_64/interrupts/timer.h>
#include <ramdisk/fat12.h>
#include <ramdisk/ramdisk.h>
#include <elf.h>


extern struct flanterm_context *ft_ctx;
extern volatile struct limine_framebuffer_request framebuffer_request;

#define SYSCALL_NULL            0
#define SYSCALL_WRITE           1
#define SYSCALL_KMALLOC         2
#define SYSCALL_KFREE           3
#define SYSCALL_WRITE_FILE      4
#define SYSCALL_READ_FILE       5
#define SYSCALL_EXEC            6
#define SYSCALL_LS              7
#define SYSCALL_SLEEP           8
#define SYSCALL_PLOT            9
#define SYSCALL_FILL_RECT       10
#define SYSCALL_GET_FB_INFO     11
#define SYSCALL_CLEAR_SCREEN    12
#define SYSCALL_GET_KEY         13

#define ERR_SUCCESS             0
#define ERR_INVALID_ARG         -1
#define ERR_FILE_NOT_FOUND      -2
#define ERR_READ_FAILED         -3
#define ERR_OUT_OF_BOUNDS       -4

typedef struct {
    char name[13];
    uint32_t size;
    uint8_t attr;
} file_info_t;

typedef struct {
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint32_t bpp;
} fb_info_t;

static volatile uint32_t last_key = 0;
static volatile bool key_available = false;

static inline struct limine_framebuffer *get_framebuffer(void) {
    return framebuffer_request.response->framebuffers[0];
}

static int64_t sys_write(uint64_t str_ptr) {
    if (!str_ptr) return ERR_INVALID_ARG;
    flanterm_write(ft_ctx, (const char *)str_ptr);
    return ERR_SUCCESS;
}

static int64_t sys_kmalloc(uint64_t size) {
    if (size == 0) return ERR_INVALID_ARG;
    void *ptr = kmalloc(size);
    return ptr ? (int64_t)ptr : ERR_INVALID_ARG;
}

static int64_t sys_kfree(uint64_t ptr) {
    if (!ptr) return ERR_INVALID_ARG;
    kfree((void *)ptr);
    return ERR_SUCCESS;
}

static int64_t sys_write_file(uint64_t filename_ptr, uint64_t data_ptr) {
    if (!filename_ptr || !data_ptr) return ERR_INVALID_ARG;
    write_file((const char *)filename_ptr, (uint8_t *)data_ptr);
    return ERR_SUCCESS;
}

static int64_t sys_read_file(uint64_t filename_ptr, uint64_t size_out_ptr) {
    if (!filename_ptr || !size_out_ptr) return ERR_INVALID_ARG;
    
    dir_entry_t *file = find_file((const char *)filename_ptr);
    if (!file) return ERR_FILE_NOT_FOUND;
    
    uint32_t size = 0;
    uint8_t *data = read_file(file, &size);
    kfree(file);
    
    if (!data || size == 0) return ERR_READ_FAILED;
    
    *(uint32_t *)size_out_ptr = size;
    return (int64_t)data;
}

static int64_t sys_exec(uint64_t filename_ptr) {
    if (!filename_ptr) return ERR_INVALID_ARG;
    return execute_elf((const char *)filename_ptr);
}

static void format_fat12_filename(char *dest, const uint8_t *name, const uint8_t *ext) {
    int pos = 0;
    for (int i = 0; i < 8 && name[i] != ' '; i++) {
        dest[pos++] = name[i];
    }
    if (ext[0] != ' ') {
        dest[pos++] = '.';
        for (int i = 0; i < 3 && ext[i] != ' '; i++) {
            dest[pos++] = ext[i];
        }
    }
    dest[pos] = '\0';
}

static int64_t sys_ls(uint64_t user_buffer_ptr, uint64_t max_entries) {
    if (!user_buffer_ptr || max_entries == 0) return ERR_INVALID_ARG;
    
    file_info_t *user_buffer = (file_info_t *)user_buffer_ptr;
    uint8_t *buffer = kmalloc(512);
    if (!buffer) return ERR_INVALID_ARG;
    
    uint32_t file_count = 0;
    
    for (uint32_t sector = 0; sector < 14 && file_count < max_entries; sector++) {
        read_ramdisk_sector(19 + sector, buffer);
        
        for (int i = 0; i < 16 && file_count < max_entries; i++) {
            dir_entry_t *entry = (dir_entry_t *)(buffer + i * 32);
            
            if (entry->name[0] == 0x00) goto done;
            if (entry->name[0] == 0xE5) continue;
            if (entry->attr & 0x08) continue;
            if (entry->attr & 0x10) continue;
            
            format_fat12_filename(user_buffer[file_count].name, entry->name, entry->ext);
            user_buffer[file_count].size = entry->file_size;
            user_buffer[file_count].attr = entry->attr;
            file_count++;
        }
    }
    
done:
    kfree(buffer);
    return file_count;
}

static int64_t sys_sleep(uint64_t time_ms) {
    if (time_ms == 0) return ERR_SUCCESS;
    timer_wait_ms(time_ms);
    return ERR_SUCCESS;
}

static int64_t sys_plot_pixel(uint64_t x, uint64_t y, uint32_t color) {
    struct limine_framebuffer *fb = get_framebuffer();
    if (!fb || !fb->address) return ERR_INVALID_ARG;
    
    if (x >= fb->width || y >= fb->height) return ERR_OUT_OF_BOUNDS;
    
    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    fb_ptr[y * (fb->pitch / 4) + x] = color;
    
    return ERR_SUCCESS;
}

static int64_t sys_fill_rect(uint64_t x, uint64_t arg2, uint64_t arg3) {
    struct limine_framebuffer *fb = get_framebuffer();
    if (!fb || !fb->address) return ERR_INVALID_ARG;
    
    uint64_t y = arg2 & 0xFFFFFFFF;
    uint64_t w = arg2 >> 32;
    uint64_t h = arg3 & 0xFFFFFFFF;
    uint32_t color = arg3 >> 32;
    
    if (x >= fb->width || y >= fb->height) return ERR_OUT_OF_BOUNDS;
    if (x + w > fb->width) w = fb->width - x;
    if (y + h > fb->height) h = fb->height - y;
    
    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    uint32_t pitch_pixels = fb->pitch / 4;
    
    for (uint64_t row = 0; row < h; row++) {
        for (uint64_t col = 0; col < w; col++) {
            fb_ptr[(y + row) * pitch_pixels + (x + col)] = color;
        }
    }
    
    return ERR_SUCCESS;
}

static int64_t sys_get_fb_info(uint64_t buffer_ptr) {
    if (!buffer_ptr) return ERR_INVALID_ARG;
    
    struct limine_framebuffer *fb = get_framebuffer();
    if (!fb) return ERR_INVALID_ARG;
    
    fb_info_t *info = (fb_info_t *)buffer_ptr;
    info->width = fb->width;
    info->height = fb->height;
    info->pitch = fb->pitch;
    info->bpp = fb->bpp;
    
    return ERR_SUCCESS;
}

static int64_t sys_clear_screen(uint32_t color) {
    struct limine_framebuffer *fb = get_framebuffer();
    if (!fb || !fb->address) return ERR_INVALID_ARG;
    
    volatile uint32_t *fb_ptr = (volatile uint32_t *)fb->address;
    uint64_t total_pixels = (fb->pitch / 4) * fb->height;
    
    for (uint64_t i = 0; i < total_pixels; i++) {
        fb_ptr[i] = color;
    }
    
    return ERR_SUCCESS;
}

void syscall_set_key(uint32_t key) {
    last_key = key;
    key_available = true;
}

static int64_t sys_get_key(void) {
    if (!key_available) return 0;
    key_available = false;
    return last_key;
}

int64_t syscall_handler(uint64_t syscall, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    switch (syscall) {
        case SYSCALL_NULL:
            return ERR_SUCCESS;
        case SYSCALL_WRITE:
            return sys_write(arg1);
        case SYSCALL_KMALLOC:
            return sys_kmalloc(arg1);
        case SYSCALL_KFREE:
            return sys_kfree(arg1);
        case SYSCALL_WRITE_FILE:
            return sys_write_file(arg1, arg2);
        case SYSCALL_READ_FILE:
            return sys_read_file(arg1, arg2);
        case SYSCALL_EXEC:
            return sys_exec(arg1);
        case SYSCALL_LS:
            return sys_ls(arg1, arg2);
        case SYSCALL_SLEEP:
            return sys_sleep(arg1);
        case SYSCALL_PLOT:
            return sys_plot_pixel(arg1, arg2, (uint32_t)arg3);
        case SYSCALL_FILL_RECT:
            return sys_fill_rect(arg1, arg2, arg3);
        case SYSCALL_GET_FB_INFO:
            return sys_get_fb_info(arg1);
        case SYSCALL_CLEAR_SCREEN:
            return sys_clear_screen((uint32_t)arg1);
        case SYSCALL_GET_KEY:
            return sys_get_key();
        default:
            return ERR_INVALID_ARG;
    }
}