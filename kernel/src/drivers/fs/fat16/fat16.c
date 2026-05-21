#include <logging/printk.h>
#include <stdint.h>
#include <stddef.h>
#include <mem.h>
#include <mm/hhdm.h>
#include <mm/heap.h>
#include <mm/frame.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/fs/fat16/fat16.h>

struct __attribute__((packed)) bpb {
    uint8_t  jmp_boot[3];
    uint8_t  oem_name[8];
    uint16_t bps;
    uint8_t  spc;
    uint16_t rsc;
    uint8_t  num_fats;
    uint16_t rec;
    uint16_t ts16;
    uint8_t  media;
    uint16_t fat_s;
    uint16_t spt;
    uint16_t heads;
    uint32_t hidden;
    uint32_t ts32;
};

struct __attribute__((packed)) ebr {
    uint8_t  drive_num;
    uint8_t  r1;
    uint8_t  bsig;
    uint32_t vol_id;
    uint8_t  vol_lab[11];
    uint8_t  f_type[8];
};

typedef struct __attribute__((packed)) {
    struct bpb bpb;
    struct ebr ebr;
} fat;

struct __attribute__((packed)) fat16_dirent {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  crtime_tenths;
    uint16_t crtime;
    uint16_t crdate;
    uint16_t ladate;
    uint16_t cluster_high;
    uint16_t wtime;
    uint16_t wdate;
    uint16_t cluster_low;
    uint32_t size;
};

#define FAT16_ATTR_READ_ONLY  0x01
#define FAT16_ATTR_HIDDEN     0x02
#define FAT16_ATTR_SYSTEM     0x04
#define FAT16_ATTR_VOLUME     0x08
#define FAT16_ATTR_DIRECTORY  0x10
#define FAT16_ATTR_ARCHIVE    0x20
#define FAT16_ATTR_LFN        0x0F

typedef struct fat_node {
    fat            data;
    vfs_blockdev_t blockdev;
    struct fat_node *next;
} fat_node;

typedef struct {
    fat_node *head;
    size_t    size;
} fat_list;

static fat_list fat16_volumes;
static uint8_t  fat16_volumes_ready = 0;

#define FAT16_LBA              0
#define FAT16_SECTOR_SIZE      512
#define FAT16_ROOT_ENTRIES     512
#define FAT16_RESERVED_SECTORS 1
#define FAT16_NUM_FATS         2
#define FAT16_MEDIA            0xF8
#define FAT16_CLUSTER_SIZE     8
#define FAT16_OEM              "MSDOS5.0"
#define FAT16_VOL_LABEL        "NO NAME    "
#define FAT16_FS_TYPE          "FAT16   "

#define FAT16_CLUSTER_FREE     0x0000
#define FAT16_CLUSTER_BAD      0xFFF7
#define FAT16_CLUSTER_LAST     0xFFF8

static void fat_list_init(fat_list *list) {
    list->head = NULL;
    list->size = 0;
}

static fat_node *fat_list_push_back(fat_list *list, const fat *data, vfs_blockdev_t *blockdev) {
    fat_node *node = kmalloc(sizeof(fat_node));
    if (!node) return NULL;

    memcpy(&node->data, data, sizeof(fat));
    node->blockdev = *blockdev;
    node->next     = NULL;

    if (!list->head) {
        list->head = node;
    } else {
        fat_node *cur = list->head;
        while (cur->next) cur = cur->next;
        cur->next = node;
    }

    list->size++;
    return node;
}

static fat_node *fat_list_find_vol_id(const fat_list *list, uint32_t vol_id) {
    for (fat_node *cur = list->head; cur; cur = cur->next)
        if (cur->data.ebr.vol_id == vol_id)
            return cur;
    return NULL;
}

static uint32_t fat16_get_fat_offset(const fat_node *vol, uint16_t cluster) {
    return vol->data.bpb.rsc + (cluster * 2) / vol->data.bpb.bps;
}

static uint16_t fat16_get_fat_index(const fat_node *vol, uint16_t cluster) {
    return (cluster * 2) % vol->data.bpb.bps;
}

static uint32_t fat16_get_cluster_sector(const fat_node *vol, uint16_t cluster) {
    uint16_t root_sectors = (vol->data.bpb.rec * 32 + vol->data.bpb.bps - 1) / vol->data.bpb.bps;
    uint32_t first_data = vol->data.bpb.rsc + vol->data.bpb.num_fats * vol->data.bpb.fat_s + root_sectors;
    return first_data + (cluster - 2) * vol->data.bpb.spc;
}

static uint16_t fat16_get_next_cluster(const fat_node *vol, uint16_t cluster) {
    uint64_t phys = frame_alloc();
    if (!phys) return 0;
    
    uint8_t *sector = (uint8_t *)phys_virt(phys);
    uint32_t fat_sector = fat16_get_fat_offset(vol, cluster);
    
    if (((fat_node *)vol)->blockdev.read(&((fat_node *)vol)->blockdev, fat_sector, 1, sector) != 0) {
        frame_free(phys);
        return 0;
    }
    
    uint16_t index = fat16_get_fat_index(vol, cluster);
    uint16_t next = *(uint16_t *)(sector + index);
    
    frame_free(phys);
    return next;
}

static uint8_t fat16_set_next_cluster(const fat_node *vol, uint16_t cluster, uint16_t next) {
    uint64_t phys = frame_alloc();
    if (!phys) return 1;
    
    uint8_t *sector = (uint8_t *)phys_virt(phys);
    uint32_t fat_sector = fat16_get_fat_offset(vol, cluster);
    
    if (((fat_node *)vol)->blockdev.read(&((fat_node *)vol)->blockdev, fat_sector, 1, sector) != 0) {
        frame_free(phys);
        return 2;
    }
    
    uint16_t index = fat16_get_fat_index(vol, cluster);
    *(uint16_t *)(sector + index) = next;
    
    for (uint8_t i = 0; i < vol->data.bpb.num_fats; i++) {
        uint32_t write_sector = vol->data.bpb.rsc + i * vol->data.bpb.fat_s + (fat_sector - vol->data.bpb.rsc) % vol->data.bpb.fat_s;
        if (((fat_node *)vol)->blockdev.write(&((fat_node *)vol)->blockdev, write_sector, 1, sector) != 0) {
            frame_free(phys);
            return 3;
        }
    }
    
    frame_free(phys);
    return 0;
}

static uint16_t fat16_allocate_cluster(const fat_node *vol) {
    for (uint16_t cluster = 2; cluster < 65525; cluster++) {
        if (fat16_get_next_cluster(vol, cluster) == FAT16_CLUSTER_FREE) {
            if (fat16_set_next_cluster(vol, cluster, FAT16_CLUSTER_LAST) == 0)
                return cluster;
        }
    }
    return 0;
}

static void fat16_free_cluster_chain(const fat_node *vol, uint16_t start_cluster) {
    uint16_t current = start_cluster;
    while (current >= 2 && current < FAT16_CLUSTER_BAD) {
        uint16_t next = fat16_get_next_cluster(vol, current);
        fat16_set_next_cluster(vol, current, FAT16_CLUSTER_FREE);
        current = next;
    }
}

static uint8_t fat16_read_cluster(const fat_node *vol, uint16_t cluster, uint8_t *buffer) {
    uint32_t sector = fat16_get_cluster_sector(vol, cluster);
    return ((fat_node *)vol)->blockdev.read(&((fat_node *)vol)->blockdev, sector, vol->data.bpb.spc, buffer);
}

static uint8_t fat16_write_cluster(const fat_node *vol, uint16_t cluster, const uint8_t *buffer) {
    uint32_t sector = fat16_get_cluster_sector(vol, cluster);
    return ((fat_node *)vol)->blockdev.write(&((fat_node *)vol)->blockdev, sector, vol->data.bpb.spc, (void *)buffer);
}

static void fat16_read_dirent(struct fat16_dirent *dirent, char *name, size_t name_len) {
    size_t pos = 0;
    for (int i = 0; i < 8 && dirent->name[i] != ' ' && dirent->name[i] != 0; i++) {
        if (pos < name_len - 1) name[pos++] = dirent->name[i];
    }
    
    if (!(dirent->attr & FAT16_ATTR_DIRECTORY)) {
        if (pos < name_len - 1) name[pos++] = '.';
        for (int i = 0; i < 3 && dirent->ext[i] != ' ' && dirent->ext[i] != 0; i++) {
            if (pos < name_len - 1) name[pos++] = dirent->ext[i];
        }
    }
    
    if (pos < name_len) name[pos] = 0;
}

static void fat16_write_dirent_name(struct fat16_dirent *dirent, const char *name) {
    memset(dirent->name, ' ', 8);
    memset(dirent->ext, ' ', 3);
    
    const char *dot = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    
    if (dot) {
        size_t name_part_len = dot - name;
        if (name_part_len > 8) name_part_len = 8;
        memcpy(dirent->name, name, name_part_len);
        
        const char *ext = dot + 1;
        size_t ext_len = 0;
        for (; ext[ext_len] && ext_len < 3; ext_len++);
        memcpy(dirent->ext, ext, ext_len);
    } else {
        size_t name_len = 0;
        for (; name[name_len] && name_len < 8; name_len++);
        memcpy(dirent->name, name, name_len);
    }
}

static struct fat16_dirent *fat16_find_dirent(const fat_node *vol, uint16_t dir_cluster, const char *name, struct fat16_dirent *result) {
    uint16_t current = dir_cluster;
    uint64_t phys = frame_alloc();
    if (!phys) return NULL;
    
    uint8_t *buffer = (uint8_t *)phys_virt(phys);
    
    while (current >= 2 && current < FAT16_CLUSTER_BAD) {
        if (fat16_read_cluster(vol, current, buffer) != 0) {
            frame_free(phys);
            return NULL;
        }
        
        for (int i = 0; i < vol->data.bpb.bps / 32; i++) {
            struct fat16_dirent *entry = (struct fat16_dirent *)(buffer + i * 32);
            
            if (entry->name[0] == 0) {
                frame_free(phys);
                return NULL;
            }
            
            if (entry->name[0] == 0xE5) continue;
            if (entry->attr == FAT16_ATTR_LFN) continue;
            
            char entry_name[256];
            fat16_read_dirent(entry, entry_name, sizeof(entry_name));
            
            if (strcmp(entry_name, name) == 0) {
                memcpy(result, entry, sizeof(struct fat16_dirent));
                frame_free(phys);
                return result;
            }
        }
        
        current = fat16_get_next_cluster(vol, current);
    }
    
    frame_free(phys);
    return NULL;
}

static uint8_t fat16_read_file_core(const fat_node *vol, uint16_t start_cluster, uint32_t size, uint8_t *buffer, uint32_t *bytes_read) {
    uint16_t current = start_cluster;
    uint32_t remaining = size;
    uint32_t bytes = 0;
    uint32_t cluster_bytes = vol->data.bpb.spc * vol->data.bpb.bps;
    
    while (current >= 2 && current < FAT16_CLUSTER_BAD && remaining > 0) {
        uint32_t to_read = remaining > cluster_bytes ? cluster_bytes : remaining;
        
        if (fat16_read_cluster(vol, current, buffer + bytes) != 0) {
            *bytes_read = bytes;
            return 1;
        }
        
        bytes += to_read;
        remaining -= to_read;
        current = fat16_get_next_cluster(vol, current);
    }
    
    *bytes_read = bytes;
    return 0;
}

static uint8_t fat16_write_file_core(const fat_node *vol, uint16_t *start_cluster, const uint8_t *buffer, uint32_t size, uint32_t *bytes_written) {
    uint32_t remaining = size;
    uint32_t bytes = 0;
    uint32_t cluster_bytes = vol->data.bpb.spc * vol->data.bpb.bps;
    uint16_t first_cluster = 0;
    uint16_t current = 0;
    
    while (remaining > 0) {
        uint16_t new_cluster = fat16_allocate_cluster(vol);
        if (!new_cluster) {
            *bytes_written = bytes;
            return 1;
        }
        
        if (!first_cluster) {
            first_cluster = new_cluster;
            current = new_cluster;
        } else {
            if (fat16_set_next_cluster(vol, current, new_cluster) != 0) {
                *bytes_written = bytes;
                return 2;
            }
            current = new_cluster;
        }
        
        uint32_t to_write = remaining > cluster_bytes ? cluster_bytes : remaining;
        uint64_t phys = frame_alloc();
        if (!phys) {
            *bytes_written = bytes;
            return 3;
        }
        
        uint8_t *cluster_buf = (uint8_t *)phys_virt(phys);
        memset(cluster_buf, 0, cluster_bytes);
        memcpy(cluster_buf, buffer + bytes, to_write);
        
        if (fat16_write_cluster(vol, current, cluster_buf) != 0) {
            frame_free(phys);
            *bytes_written = bytes;
            return 4;
        }
        
        frame_free(phys);
        bytes += to_write;
        remaining -= to_write;
    }
    
    *start_cluster = first_cluster;
    *bytes_written = bytes;
    return 0;
}

static uint8_t fat16_create_dirent(const fat_node *vol, uint16_t dir_cluster, const char *name, uint8_t attr, uint16_t start_cluster, uint32_t size) {
    uint64_t phys = frame_alloc();
    if (!phys) return 1;
    
    uint8_t *buffer = (uint8_t *)phys_virt(phys);
    uint16_t current = dir_cluster;
    uint16_t free_cluster = 0;
    int free_index = -1;
    
    while (current >= 2 && current < FAT16_CLUSTER_BAD) {
        if (fat16_read_cluster(vol, current, buffer) != 0) {
            frame_free(phys);
            return 2;
        }
        
        for (int i = 0; i < vol->data.bpb.bps / 32; i++) {
            struct fat16_dirent *entry = (struct fat16_dirent *)(buffer + i * 32);
            
            if (entry->name[0] == 0 || entry->name[0] == 0xE5) {
                if (free_index == -1) {
                    free_cluster = current;
                    free_index = i;
                }
            }
        }
        
        if (free_index != -1) break;
        current = fat16_get_next_cluster(vol, current);
    }
    
    if (free_index == -1) {
        uint16_t new_cluster = fat16_allocate_cluster(vol);
        if (!new_cluster) {
            frame_free(phys);
            return 3;
        }
        
        if (current >= 2 && current < FAT16_CLUSTER_BAD) {
            if (fat16_set_next_cluster(vol, current, new_cluster) != 0) {
                frame_free(phys);
                return 4;
            }
        }
        
        memset(buffer, 0, vol->data.bpb.spc * vol->data.bpb.bps);
        free_cluster = new_cluster;
        free_index = 0;
    }
    
    if (fat16_read_cluster(vol, free_cluster, buffer) != 0) {
        frame_free(phys);
        return 5;
    }
    
    struct fat16_dirent *entry = (struct fat16_dirent *)(buffer + free_index * 32);
    memset(entry, 0, 32);
    fat16_write_dirent_name(entry, name);
    entry->attr = attr;
    entry->cluster_low = start_cluster & 0xFFFF;
    entry->cluster_high = (start_cluster >> 16) & 0xFFFF;
    entry->size = size;
    
    if (fat16_write_cluster(vol, free_cluster, buffer) != 0) {
        frame_free(phys);
        return 6;
    }
    
    frame_free(phys);
    return 0;
}

static uint8_t fat16_delete_dirent(const fat_node *vol, uint16_t dir_cluster, const char *name) {
    uint64_t phys = frame_alloc();
    if (!phys) return 1;
    
    uint8_t *buffer = (uint8_t *)phys_virt(phys);
    uint16_t current = dir_cluster;
    
    while (current >= 2 && current < FAT16_CLUSTER_BAD) {
        if (fat16_read_cluster(vol, current, buffer) != 0) {
            frame_free(phys);
            return 2;
        }
        
        for (int i = 0; i < vol->data.bpb.bps / 32; i++) {
            struct fat16_dirent *entry = (struct fat16_dirent *)(buffer + i * 32);
            
            if (entry->name[0] == 0) {
                frame_free(phys);
                return 3;
            }
            
            if (entry->name[0] == 0xE5 || entry->attr == FAT16_ATTR_LFN) continue;
            
            char entry_name[256];
            fat16_read_dirent(entry, entry_name, sizeof(entry_name));
            
            if (strcmp(entry_name, name) == 0) {
                entry->name[0] = 0xE5;
                
                uint16_t cluster = entry->cluster_low | (entry->cluster_high << 16);
                if (!(entry->attr & FAT16_ATTR_DIRECTORY) && cluster) {
                    fat16_free_cluster_chain(vol, cluster);
                }
                
                if (fat16_write_cluster(vol, current, buffer) != 0) {
                    frame_free(phys);
                    return 4;
                }
                
                frame_free(phys);
                return 0;
            }
        }
        
        current = fat16_get_next_cluster(vol, current);
    }
    
    frame_free(phys);
    return 4;
}

static void fat16_debug_print(const fat *f) {
    char oem[9]  = {0};
    char lab[12] = {0};
    char type[9] = {0};

    memcpy(oem,  f->bpb.oem_name, 8);
    memcpy(lab,  f->ebr.vol_lab,  11);
    memcpy(type, f->ebr.f_type,   8);

    log_info("FAT16 BPB: oem='%s' bps=%u spc=%u rsc=%u fats=%u root=%u ts16=%u media=0x%02x fat_s=%u hidden=%u ts32=%u",
             oem, f->bpb.bps, f->bpb.spc, f->bpb.rsc, f->bpb.num_fats,
             f->bpb.rec, f->bpb.ts16, f->bpb.media, f->bpb.fat_s,
             f->bpb.hidden, f->bpb.ts32);

    log_info("FAT16 EBR: sig=0x%02x vol_id=0x%08x label='%s' type='%s'",
             f->ebr.bsig, f->ebr.vol_id, lab, type);
}

static fat *read_fat(vfs_blockdev_t *dev) {
    uint64_t phys = frame_alloc();
    if (!phys) return NULL;

    void *sector = (void *)phys_virt(phys);

    if (dev->read(dev, FAT16_LBA, 1, sector) != 0) {
        frame_free(phys);
        return NULL;
    }

    fat *f = kmalloc(sizeof(fat));
    if (!f) {
        frame_free(phys);
        return NULL;
    }

    memcpy(f, sector, sizeof(fat));
    frame_free(phys);
    return f;
}

static uint16_t fat16_compute_fat_size(uint32_t total_sectors) {
    uint16_t root_dir_sectors = (FAT16_ROOT_ENTRIES * 32 + FAT16_SECTOR_SIZE - 1) / FAT16_SECTOR_SIZE;
    uint16_t fat_size = 1;
    for (;;) {
        uint32_t data_sectors = total_sectors - FAT16_RESERVED_SECTORS - FAT16_NUM_FATS * fat_size - root_dir_sectors;
        if ((int32_t)data_sectors <= 0) return 0;
        uint32_t cluster_count = data_sectors / FAT16_CLUSTER_SIZE;
        if (cluster_count < 4085 || cluster_count >= 65525) return 0;
        uint16_t next = (uint16_t)((cluster_count * 2 + FAT16_SECTOR_SIZE - 1) / FAT16_SECTOR_SIZE);
        if (next == fat_size) return fat_size;
        fat_size = next;
    }
}

static uint8_t fat16_validate(const fat *f) {
    const struct bpb *b = &f->bpb;
    if (b->bps < 512 || b->bps > 4096 || (b->bps & (b->bps - 1))) return 1;
    if (!b->spc || (b->spc & (b->spc - 1)))                         return 2;
    if (b->rsc < 1)                                                  return 3;
    if (b->num_fats < 1)                                             return 4;
    if (b->fat_s == 0)                                               return 5;
    if (b->ts16 == 0 && b->ts32 == 0)                               return 6;
    if (f->ebr.bsig != 0x29)                                         return 7;
    if (f->ebr.vol_id == 0)                                          return 8;
    return 0;
}

uint8_t fat16_format(vfs_blockdev_t *dev, uint32_t total_sectors) {
    uint16_t fat_size = fat16_compute_fat_size(total_sectors);
    if (!fat_size) return 1;

    uint64_t phys = frame_alloc();
    if (!phys) return 2;

    uint8_t *sector = (uint8_t *)phys_virt(phys);
    memset(sector, 0, FAT16_SECTOR_SIZE);

    sector[0] = 0xEB; sector[1] = 0x3C; sector[2] = 0x90;
    memcpy(sector + 3, FAT16_OEM, 8);
    sector[11] = FAT16_SECTOR_SIZE & 0xFF;
    sector[12] = FAT16_SECTOR_SIZE >> 8;
    sector[13] = FAT16_CLUSTER_SIZE;
    sector[14] = FAT16_RESERVED_SECTORS & 0xFF;
    sector[15] = FAT16_RESERVED_SECTORS >> 8;
    sector[16] = FAT16_NUM_FATS;
    sector[17] = FAT16_ROOT_ENTRIES & 0xFF;
    sector[18] = FAT16_ROOT_ENTRIES >> 8;
    if (total_sectors <= 0xFFFF) {
        sector[19] = total_sectors & 0xFF;
        sector[20] = total_sectors >> 8;
    }
    sector[21] = FAT16_MEDIA;
    sector[22] = fat_size & 0xFF;
    sector[23] = fat_size >> 8;
    sector[24] = 63;
    sector[25] = 0;
    sector[26] = 255;
    sector[27] = 0;
    sector[28] = 0; sector[29] = 0; sector[30] = 0; sector[31] = 0;
    if (total_sectors > 0xFFFF) {
        sector[32] = total_sectors & 0xFF;
        sector[33] = (total_sectors >> 8)  & 0xFF;
        sector[34] = (total_sectors >> 16) & 0xFF;
        sector[35] = (total_sectors >> 24) & 0xFF;
    }
    sector[36] = 0x80;
    sector[37] = 0;
    sector[38] = 0x29;
    uint32_t vol_id = 0x12345678;
    sector[39] = vol_id & 0xFF;
    sector[40] = (vol_id >> 8)  & 0xFF;
    sector[41] = (vol_id >> 16) & 0xFF;
    sector[42] = (vol_id >> 24) & 0xFF;
    memcpy(sector + 43, FAT16_VOL_LABEL, 11);
    memcpy(sector + 54, FAT16_FS_TYPE,   8);
    sector[510] = 0x55;
    sector[511] = 0xAA;

    if (dev->write(dev, FAT16_LBA, 1, sector) != 0) {
        frame_free(phys);
        return 3;
    }

    memset(sector, 0, FAT16_SECTOR_SIZE);
    sector[0] = FAT16_MEDIA;
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    sector[3] = 0xFF;

    for (uint8_t i = 0; i < FAT16_NUM_FATS; i++) {
        uint32_t base = FAT16_RESERVED_SECTORS + i * fat_size;
        if (dev->write(dev, base, 1, sector) != 0) {
            frame_free(phys);
            return 4;
        }
        memset(sector, 0, FAT16_SECTOR_SIZE);
        for (uint16_t j = 1; j < fat_size; j++) {
            if (dev->write(dev, base + j, 1, sector) != 0) {
                frame_free(phys);
                return 5;
            }
        }
    }

    memset(sector, 0, FAT16_SECTOR_SIZE);
    uint16_t root_dir_sectors = (FAT16_ROOT_ENTRIES * 32 + FAT16_SECTOR_SIZE - 1) / FAT16_SECTOR_SIZE;
    uint32_t root_base = FAT16_RESERVED_SECTORS + FAT16_NUM_FATS * fat_size;
    for (uint16_t i = 0; i < root_dir_sectors; i++) {
        if (dev->write(dev, root_base + i, 1, sector) != 0) {
            frame_free(phys);
            return 6;
        }
    }

    frame_free(phys);
    return 0;
}

uint8_t fat16_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buffer) {
    return dev->read(dev, lba, count, buffer);
}

void *fat16_init(vfs_blockdev_t *dev) {
    if (!fat16_volumes_ready) {
        fat_list_init(&fat16_volumes);
        fat16_volumes_ready = 1;
    }

    fat *f = read_fat(dev);
    if (!f) return NULL;

    fat16_debug_print(f);

    if (fat16_validate(f) != 0) {
        kfree(f);
        return NULL;
    }

    if (fat_list_find_vol_id(&fat16_volumes, f->ebr.vol_id)) {
        kfree(f);
        return NULL;
    }

    fat_node *node = fat_list_push_back(&fat16_volumes, f, dev);
    if (!node) {
        kfree(f);
        return NULL;
    }

    kfree(f);
    return (void *)node;
}

uint8_t fat16_read_file(const void *vol_ptr, uint16_t start_cluster, uint32_t size, uint8_t *buffer, uint32_t *bytes_read) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) return 1;
    return fat16_read_file_core(vol, start_cluster, size, buffer, bytes_read);
}

uint8_t fat16_write_file(const void *vol_ptr, uint16_t *start_cluster, const uint8_t *buffer, uint32_t size, uint32_t *bytes_written) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) return 1;
    return fat16_write_file_core(vol, start_cluster, buffer, size, bytes_written);
}

uint8_t fat16_create_file(const void *vol_ptr, uint16_t dir_cluster, const char *name, const uint8_t *buffer, uint32_t size) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) return 1;
    
    uint16_t start_cluster = 0;
    uint32_t bytes_written = 0;
    
    if (size > 0) {
        if (fat16_write_file_core(vol, &start_cluster, buffer, size, &bytes_written) != 0)
            return 2;
    }
    
    if (fat16_create_dirent(vol, dir_cluster, name, FAT16_ATTR_ARCHIVE, start_cluster, size) != 0) {
        if (start_cluster) fat16_free_cluster_chain(vol, start_cluster);
        return 3;
    }
    
    return 0;
}

uint8_t fat16_delete_file(const void *vol_ptr, uint16_t dir_cluster, const char *name) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) return 1;
    
    return fat16_delete_dirent(vol, dir_cluster, name);
}

uint8_t fat16_create_directory(const void *vol_ptr, uint16_t parent_cluster, const char *name) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) return 1;
    
    uint16_t new_cluster = fat16_allocate_cluster(vol);
    if (!new_cluster) return 2;
    
    uint64_t phys = frame_alloc();
    if (!phys) {
        fat16_free_cluster_chain(vol, new_cluster);
        return 3;
    }
    
    uint8_t *buffer = (uint8_t *)phys_virt(phys);
    memset(buffer, 0, vol->data.bpb.spc * vol->data.bpb.bps);
    
    if (fat16_write_cluster(vol, new_cluster, buffer) != 0) {
        frame_free(phys);
        fat16_free_cluster_chain(vol, new_cluster);
        return 4;
    }
    
    frame_free(phys);
    
    if (fat16_create_dirent(vol, parent_cluster, name, FAT16_ATTR_DIRECTORY, new_cluster, 0) != 0) {
        fat16_free_cluster_chain(vol, new_cluster);
        return 5;
    }
    
    return 0;
}

uint8_t fat16_delete_directory(const void *vol_ptr, uint16_t parent_cluster, const char *name) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) return 1;
    
    struct fat16_dirent dirent;
    if (!fat16_find_dirent(vol, parent_cluster, name, &dirent)) return 2;
    
    if (!(dirent.attr & FAT16_ATTR_DIRECTORY)) return 3;
    
    uint16_t dir_cluster = dirent.cluster_low | (dirent.cluster_high << 16);
    
    uint64_t phys = frame_alloc();
    if (!phys) return 4;
    
    uint8_t *buffer = (uint8_t *)phys_virt(phys);
    uint16_t current = dir_cluster;
    uint8_t is_empty = 1;
    
    while (current >= 2 && current < FAT16_CLUSTER_BAD) {
        if (fat16_read_cluster(vol, current, buffer) != 0) {
            frame_free(phys);
            return 5;
        }
        
        for (int i = 0; i < vol->data.bpb.bps / 32; i++) {
            struct fat16_dirent *entry = (struct fat16_dirent *)(buffer + i * 32);
            
            if (entry->name[0] == 0 || entry->name[0] == 0xE5) continue;
            if (entry->attr == FAT16_ATTR_LFN) continue;
            
            is_empty = 0;
            break;
        }
        
        if (!is_empty) break;
        current = fat16_get_next_cluster(vol, current);
    }
    
    frame_free(phys);
    
    if (!is_empty) return 6;
    
    fat16_free_cluster_chain(vol, dir_cluster);
    
    return fat16_delete_dirent(vol, parent_cluster, name);
}

uint8_t fat16_list_directory(const void *vol_ptr, uint16_t dir_cluster, fat16_dir_entry *entries, uint16_t *count) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) return 1;
    
    uint64_t phys = frame_alloc();
    if (!phys) return 2;
    
    uint8_t *buffer = (uint8_t *)phys_virt(phys);
    uint16_t current = dir_cluster;
    uint16_t index = 0;
    uint16_t max_count = *count;
    
    while (current >= 2 && current < FAT16_CLUSTER_BAD && index < max_count) {
        if (fat16_read_cluster(vol, current, buffer) != 0) {
            frame_free(phys);
            return 3;
        }
        
        for (int i = 0; i < vol->data.bpb.bps / 32 && index < max_count; i++) {
            struct fat16_dirent *entry = (struct fat16_dirent *)(buffer + i * 32);
            
            if (entry->name[0] == 0) {
                frame_free(phys);
                *count = index;
                return 0;
            }
            
            if (entry->name[0] == 0xE5 || entry->attr == FAT16_ATTR_LFN) continue;
            
            fat16_read_dirent(entry, entries[index].name, sizeof(entries[index].name));
            entries[index].start_cluster = entry->cluster_low | (entry->cluster_high << 16);
            entries[index].attr = entry->attr;
            index++;
        }
        
        current = fat16_get_next_cluster(vol, current);
    }
    
    frame_free(phys);
    *count = index;
    return 0;
}

uint8_t fat16_find_file(const void *vol_ptr, uint16_t dir_cluster, const char *name, fat16_file_handle *handle) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) return 1;
    if (!handle) return 2;
    
    struct fat16_dirent dirent;
    if (!fat16_find_dirent(vol, dir_cluster, name, &dirent)) return 3;
    
    handle->start_cluster = dirent.cluster_low | (dirent.cluster_high << 16);
    handle->size = dirent.size;
    handle->attr = dirent.attr;
    fat16_read_dirent(&dirent, handle->name, sizeof(handle->name));
    
    return 0;
}

uint32_t fat16_get_volume_id(const void *vol_ptr) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) return 0;
    return vol->data.ebr.vol_id;
}