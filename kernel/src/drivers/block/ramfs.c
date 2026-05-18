#include <limine.h>
#include <mem.h>
#include <logging/printk.h>
#include <mm/heap.h>
#include <drivers/block/ramfs.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

void *ramfs_addr;
uint64_t ramfs_size;

static unsigned long octal_to_ulong(const unsigned char *str, int size) {
    unsigned long val = 0;
    int i = 0;
    while (i < size && (str[i] == ' ' || str[i] == '\0')) i++;
    for (; i < size; i++) {
        unsigned char c = str[i];
        if (c < '0' || c > '7') break;
        val = (val << 3) + (c - '0');
    }
    return val;
}

static void copy_field(char *dest, const unsigned char *src, int size, int dest_size) {
    int i = 0;
    for (; i < size && i + 1 < dest_size; i++) {
        if (src[i] == '\0') break;
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

static unsigned long tar_lookup(unsigned char *archive, const char *filename, char **out) {
    unsigned char *ptr = archive;
    for (;;) {
        int allzero = 1;
        for (int i = 0; i < 512; i++) {
            if (ptr[i]) { allzero = 0; break; }
        }
        if (allzero) break;

        unsigned char type = ptr[156];
        unsigned long size = octal_to_ulong(ptr + 124, 12);

        char fullname[512];
        fullname[0] = '\0';

        if (type == 'L' || type == 'K') {
            unsigned char *nameptr = ptr + 512;
            int namelen = (int)size;
            if (namelen >= (int)sizeof(fullname)) namelen = sizeof(fullname) - 1;
            memcpy(fullname, nameptr, namelen);
            fullname[namelen] = '\0';
            ptr += (((size + 511) / 512) + 1) * 512;
            type = ptr[156];
            size = octal_to_ulong(ptr + 124, 12);
        }

        if (fullname[0] == '\0') {
            char name[101];
            char prefix[156];
            copy_field(name, ptr + 0, 100, sizeof(name));
            copy_field(prefix, ptr + 345, 155, sizeof(prefix));

            if (prefix[0]) {
                int plen = strlen(prefix);
                int nlen = strlen(name);
                if (plen + 1 + nlen >= (int)sizeof(fullname)) {
                    memcpy(fullname, prefix, sizeof(fullname) - 1);
                    fullname[sizeof(fullname) - 1] = '\0';
                } else {
                    memcpy(fullname, prefix, plen);
                    fullname[plen] = '/';
                    memcpy(fullname + plen + 1, name, nlen);
                    fullname[plen + 1 + nlen] = '\0';
                }
            } else {
                copy_field(fullname, ptr + 0, 100, sizeof(fullname));
            }
        }

        if (fullname[0] && (type == '0' || type == '\0' || type == '7')) {
            if (!memcmp(fullname, filename, strlen(filename) + 1)) {
                *out = (char *)(ptr + 512);
                return size;
            }
        }

        ptr += (((size + 511) / 512) + 1) * 512;
    }

    return 0;
}

void ramfs_init() {
    struct limine_module_response *response = module_request.response;
    if (!response || response->module_count == 0) {
        log_error("No modules found for ramfs\n");
        return;
    }

    ramfs_addr = response->modules[0]->address;
    ramfs_size = response->modules[0]->size;
    log_info("Ramfs module found at %llx with size %lu\n", (void *)ramfs_addr, ramfs_size);
}

ramfs_file_t *ramfs_read(char *archive, char *filename) {
    char *data;
    unsigned long size = tar_lookup((unsigned char *)archive, filename, &data);
    if (size == 0) {
        log_error("File not found in ramfs: %s\n", filename);
        return NULL;
    }

    ramfs_file_t *file = kmalloc(sizeof(ramfs_file_t));
    if (!file) {
        log_error("Failed to allocate ramfs_file_t\n");
        return NULL;
    }

    int name_len = strlen(filename) + 1;
    file->name = kmalloc(name_len);
    if (!file->name) {
        log_error("Failed to allocate filename\n");
        kfree(file);
        return NULL;
    }
    memcpy(file->name, filename, name_len);

    file->data = kmalloc((int)size);
    if (!file->data) {
        log_error("Failed to allocate file data (%lu bytes)\n", size);
        kfree(file->name);
        kfree(file);
        return NULL;
    }

    memcpy(file->data, data, (int)size);
    file->size = (int)size;
    log_info("Loaded file %s (%lu bytes)\n", filename, size);
    return file;
}

int ramfs_list(char *archive, char *directory, char **filenames, int max_files) {
    unsigned char *ptr = (unsigned char *)archive;
    int count = 0;
    int dir_len = strlen(directory);

    for (;;) {
        int allzero = 1;
        for (int i = 0; i < 512; i++) {
            if (ptr[i]) { allzero = 0; break; }
        }
        if (allzero) break;

        unsigned char type = ptr[156];
        unsigned long size = octal_to_ulong(ptr + 124, 12);

        char fullname[512];
        fullname[0] = '\0';

        if (type == 'L' || type == 'K') {
            unsigned char *nameptr = ptr + 512;
            int namelen = (int)size;
            if (namelen >= (int)sizeof(fullname)) namelen = sizeof(fullname) - 1;
            memcpy(fullname, nameptr, namelen);
            fullname[namelen] = '\0';
            ptr += (((size + 511) / 512) + 1) * 512;
            type = ptr[156];
            size = octal_to_ulong(ptr + 124, 12);
        }

        if (fullname[0] == '\0') {
            char name[101];
            char prefix[156];
            copy_field(name, ptr + 0, 100, sizeof(name));
            copy_field(prefix, ptr + 345, 155, sizeof(prefix));

            if (prefix[0]) {
                int plen = strlen(prefix);
                int nlen = strlen(name);
                if (plen + 1 + nlen >= (int)sizeof(fullname)) {
                    memcpy(fullname, prefix, sizeof(fullname) - 1);
                    fullname[sizeof(fullname) - 1] = '\0';
                } else {
                    memcpy(fullname, prefix, plen);
                    fullname[plen] = '/';
                    memcpy(fullname + plen + 1, name, nlen);
                    fullname[plen + 1 + nlen] = '\0';
                }
            } else {
                copy_field(fullname, ptr + 0, 100, sizeof(fullname));
            }
        }

        if (fullname[0]) {
            if (dir_len == 0 || !memcmp(fullname, directory, dir_len)) {
                if (type == '0' || type == '\0' || type == '7') {
                    if (count < max_files) {
                        int name_len = strlen(fullname) + 1;
                        filenames[count] = kmalloc(name_len);
                        if (filenames[count]) {
                            memcpy(filenames[count], fullname, name_len);
                            count++;
                        }
                    }
                }
            }
        }

        ptr += (((size + 511) / 512) + 1) * 512;
    }

    return count;
}