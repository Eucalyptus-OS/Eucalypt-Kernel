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

uint8_t fat16_init(vfs_blockdev_t *dev) {
    if (!fat16_volumes_ready) {
        fat_list_init(&fat16_volumes);
        fat16_volumes_ready = 1;
    }

    fat *f = read_fat(dev);
    if (!f) return 1;

    fat16_debug_print(f);

    if (fat16_validate(f) != 0) {
        kfree(f);
        return 2;
    }

    if (fat_list_find_vol_id(&fat16_volumes, f->ebr.vol_id)) {
        kfree(f);
        return 3;
    }

    if (!fat_list_push_back(&fat16_volumes, f, dev)) {
        kfree(f);
        return 4;
    }

    kfree(f);
    return 0;
}