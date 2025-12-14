#include <ramdisk/ramfs.h>
#include <ramdisk/ramdisk.h>
#include <x86_64/allocator/heap.h>
#include <x86_64/serial.h>

#include <string.h>
#include <stdint.h>
#include <stddef.h>

void init_ramfs(file_system_t *fs) {
    fs->files = NULL;
    fs->file_count = 0;
    return;
}

void list_files(file_system_t *fs, void *buf) {
    if (!fs) {
        return;
    }

    if (fs->file_count == 0) {
        serial_print("No files in RAMFS\n");
        return;
    }

    for (uint32_t i = 0; i < fs->file_count; i++) {
        serial_print(fs->files[i].name);
        memcpy(buf + i * MAX_NAME_LENGTH, fs->files[i].name, MAX_NAME_LENGTH);
        serial_putchar('\n');
    }

    return;
}

void read_file(const char *name, void *buf, uint32_t size, file_system_t *fs) {
    if (!fs || !name || !buf) {
        return;
    }

    for (uint32_t i = 0; i < fs->file_count; i++) {
        if (strncmp(fs->files[i].name, name, MAX_NAME_LENGTH) == 0) {
            uint32_t copy = (size < fs->files[i].size) ? size : fs->files[i].size;
            memcpy(buf, fs->files[i].data, copy);
            return;
        }
    }
}

void write_file(const char *name, const uint8_t *data, uint32_t size, file_system_t *fs) {
    if (!fs || !name) {
        return;
    }

    file_t *new_file = (file_t *)kmalloc(sizeof(file_t));
    if (!new_file) {
        return;
    }

    for (int c = 0; c < MAX_NAME_LENGTH - 1; c++) {
        new_file->name[c] = name[c];
        if (name[c] == '\0') {
            break;
        }
    }
    new_file->name[MAX_NAME_LENGTH - 1] = '\0';

    new_file->data = (uint8_t *)kmalloc(size);
    if (!new_file->data) {
        kfree(new_file);
        return;
    }
    new_file->size = size;
    for (uint32_t d = 0; d < size; d++) {
        new_file->data[d] = data[d];
    }

    file_t *updated_files = (file_t *)kmalloc(sizeof(file_t) * (fs->file_count + 1));
    if (!updated_files) {
        kfree(new_file->data);
        kfree(new_file);
        return;
    }

    for (uint32_t f = 0; f < fs->file_count; f++) {
        updated_files[f] = fs->files[f];
    }

    updated_files[fs->file_count] = *new_file;
    kfree(fs->files);
    fs->files = updated_files;
    fs->file_count += 1;
    kfree(new_file);
    return;
}