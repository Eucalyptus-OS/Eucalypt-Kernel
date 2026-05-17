#include <mm/heap.h>
#include <mem.h>
#include <drivers/block/ramfs.h>

static int oct2bin(unsigned char *str, int size) {
    int n = 0;
    unsigned char *c = str;
    while (size-- > 0) {
        n *= 8;
        n += *c - '0';
        c++;
    }
    return n;
}

static int tar_lookup(unsigned char *archive, char *filename, char **out) {
    unsigned char *ptr = archive;

    while (!memcmp(ptr + 257, "ustar", 5)) {
        int filesize = oct2bin(ptr + 0x7c, 11);
        if (!memcmp(ptr, filename, strlen(filename) + 1)) {
            *out = (char *)(ptr + 512);
            return filesize;
        }
        ptr += (((filesize + 511) / 512) + 1) * 512;
    }
    return 0;
}

ramfs_file_t *ramfs_read(char *archive, char *filename) {
    char *data;
    int size = tar_lookup((unsigned char *)archive, filename, &data);
    if (size <= 0) return NULL;

    ramfs_file_t *file = kmalloc(sizeof(ramfs_file_t));
    if (!file) return NULL;

    int name_len = strlen(filename) + 1;
    file->name = kmalloc(name_len);
    if (!file->name) {
        kfree(file);
        return NULL;
    }
    memcpy(file->name, filename, name_len);

    file->data = kmalloc(size);
    if (!file->data) {
        kfree(file->name);
        kfree(file);
        return NULL;
    }

    memcpy(file->data, data, size);
    return file;
}