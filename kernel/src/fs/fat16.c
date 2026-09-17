#include <stdint.h>
#include <stddef.h>
#include <mm/memory.h>
#include <mm/hhdm.h>
#include <mm/heap.h>
#include <mm/frame.h>
#include <logging/print.h>
#include <fs/vfs.h>
#include <fs/fat16.h>

// BIOS Parameter Block at sector 0; all offsets are absolute within the 512-byte boot sector
struct __attribute__((packed)) bpb {
    uint8_t  jmp_boot[3];       // x86 jump to the boot code
    uint8_t  oem_name[8];
    uint16_t bps;               // bytes per sector (0Bh)
    uint8_t  spc;               // sectors per cluster
    uint16_t rsc;               // reserved sectors before the first FAT
    uint8_t  num_fats;          // number of FAT copies
    uint16_t rec;               // root directory entries (FAT12/16 only)
    uint16_t ts16;              // total sectors, 16-bit form (0 for >=65536)
    uint8_t  media;             // media descriptor byte
    uint16_t fat_s;             // sectors per FAT
    uint16_t spt;               // sectors per track
    uint16_t heads;             // head count
    uint32_t hidden;            // hidden sectors before the partition
    uint32_t ts32;              // total sectors, 32-bit form
};

// Extended Boot Record: the boot sector bytes past the BPB, holding the volume id
struct __attribute__((packed)) ebr {
    uint8_t  drive_num;
    uint8_t  r1;
    uint8_t  bsig;              // boot signature, 0x29 for a valid EBR
    uint32_t vol_id;            // serial number used to fingerprint this volume
    uint8_t  vol_lab[11];
    uint8_t  f_type[8];         // "FAT12   "/"FAT16   " ASCII marker
};

typedef struct __attribute__((packed)) {
    struct bpb bpb;
    struct ebr ebr;
} fat;

// Fixed 32-byte directory entry as laid out on disk
struct __attribute__((packed)) fat16_dirent {
    uint8_t  name[8];           // upper-cased, space-padded 8.3 base name
    uint8_t  ext[3];            // space-padded 3-char extension
    uint8_t  attr;              // FAT16_ATTR_* flags (0x0F = LFN slot)
    uint8_t  reserved;
    uint8_t  crtime_tenths;     // reused as the LFN entry checksum
    uint16_t crtime;            // in LFN entries these 6 fields hold UCS-2 name chars 5-10
    uint16_t crdate;
    uint16_t ladate;
    uint16_t cluster_high;      // top 16 bits of the start cluster
    uint16_t wtime;
    uint16_t wdate;
    uint16_t cluster_low;       // low 16 bits of the start cluster
    uint32_t size;              // file size in bytes
};

#define FAT16_ATTR_READ_ONLY  0x01
#define FAT16_ATTR_HIDDEN     0x02
#define FAT16_ATTR_SYSTEM     0x04
#define FAT16_ATTR_VOLUME     0x08
#define FAT16_ATTR_DIRECTORY  0x10   // set on subdirectory entries
#define FAT16_ATTR_ARCHIVE    0x20
#define FAT16_ATTR_LFN        0x0F   // entry is a long-filename slot, not a file

// In-memory volume handle: parsed BPB/EBR plus the block device it lives on
typedef struct fat_node {
    fat            data;
    vfs_blockdev_t blockdev;
    struct fat_node *next;      // singleton list of open volumes
} fat_node;

typedef struct {
    fat_node *head;
    size_t    size;
} fat_list;

static fat_list fat16_volumes;
static uint8_t  fat16_volumes_ready = 0;

#define FAT16_LBA              0      // volume bootable region starts at LBA 0
#define FAT16_SECTOR_SIZE      512
#define FAT16_ROOT_ENTRIES     512    // root dir capacity used when formatting
#define FAT16_RESERVED_SECTORS 1
#define FAT16_NUM_FATS         2
#define FAT16_MEDIA            0xF8   // fixed-disk media descriptor
#define FAT16_CLUSTER_SIZE     8      // sectors per cluster used by the formatter
#define FAT16_OEM              "MSDOS5.0"
#define FAT16_VOL_LABEL        "NO NAME    "
#define FAT16_FS_TYPE          "FAT16   "

// FAT entry values: 0 = free, 0xFFF7 = bad, >= 0xFFF8 = end-of-chain
#define FAT16_CLUSTER_FREE     0x0000
#define FAT16_CLUSTER_BAD      0xFFF7
#define FAT16_CLUSTER_LAST     0xFFF8

static void fat_list_init(fat_list *list) {
    list->head = NULL;
    list->size = 0;
}

// Append a freshly parsed volume to the open-volume list
static fat_node *fat_list_push_back(fat_list *list, const fat *data, vfs_blockdev_t *blockdev) {
    fat_node *node = kmalloc(sizeof(fat_node));
    if (!node) {
        return NULL;
    }

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
    for (fat_node *cur = list->head; cur; cur = cur->next) {
        if (cur->data.ebr.vol_id == vol_id) {
            return cur;
        }
    }
    return NULL;
}

// Sector number of the FAT region holding |cluster|'s 2-byte entry: reserved + entry/512
static uint32_t fat16_get_fat_offset(const fat_node *vol, uint16_t cluster) {
    return vol->data.bpb.rsc + (cluster * 2) / vol->data.bpb.bps;
}

// Byte offset of |cluster|'s 2-byte entry inside its FAT sector
static uint16_t fat16_get_fat_index(const fat_node *vol, uint16_t cluster) {
    return (cluster * 2) % vol->data.bpb.bps;
}

// First sector of |cluster|'s data: root dir ends and data starts at cluster 2
static uint32_t fat16_get_cluster_sector(const fat_node *vol, uint16_t cluster) {
    uint16_t root_sectors = (vol->data.bpb.rec * 32 + vol->data.bpb.bps - 1) / vol->data.bpb.bps;
    uint32_t first_data = vol->data.bpb.rsc + vol->data.bpb.num_fats * vol->data.bpb.fat_s + root_sectors;
    return first_data + (cluster - 2) * vol->data.bpb.spc;
}

// Follow a single link in the FAT chain: next cluster for |cluster|
static uint16_t fat16_get_next_cluster(const fat_node *vol, uint16_t cluster) {
    uint64_t phys = frame_alloc();
    if (!phys) {
        return 0;
    }
    
    uint8_t *sector = (uint8_t *)phys_to_virt(phys);
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

// Write one FAT entry, mirroring it across every FAT copy on the volume
static uint8_t fat16_set_next_cluster(const fat_node *vol, uint16_t cluster, uint16_t next) {
    uint64_t phys = frame_alloc();
    if (!phys) {
        return 1;
    }
    
    uint8_t *sector = (uint8_t *)phys_to_virt(phys);
    uint32_t fat_sector = fat16_get_fat_offset(vol, cluster);
    
    if (((fat_node *)vol)->blockdev.read(&((fat_node *)vol)->blockdev, fat_sector, 1, sector) != 0) {
        frame_free(phys);
        return 2;
    }
    
    uint16_t index = fat16_get_fat_index(vol, cluster);
    *(uint16_t *)(sector + index) = next;
    
    for (uint8_t i = 0; i < vol->data.bpb.num_fats; i++) {
        // Same entry offset within each FAT: base + i*fat_s + offset-into-fat
        uint32_t write_sector = vol->data.bpb.rsc + i * vol->data.bpb.fat_s + (fat_sector - vol->data.bpb.rsc) % vol->data.bpb.fat_s;
        if (((fat_node *)vol)->blockdev.write(&((fat_node *)vol)->blockdev, write_sector, 1, sector) != 0) {
            frame_free(phys);
            return 3;
        }
    }
    
    frame_free(phys);
    return 0;
}

// Scan the FAT for a free cluster and mark it as end-of-chain
static uint16_t fat16_allocate_cluster(const fat_node *vol) {
    for (uint16_t cluster = 2; cluster < 65525; cluster++) {
        if (fat16_get_next_cluster(vol, cluster) == FAT16_CLUSTER_FREE) {
            if (fat16_set_next_cluster(vol, cluster, FAT16_CLUSTER_LAST) == 0) {
                return cluster;
            }
        }
    }
    return 0;
}

// Walk a chain from |start_cluster| freeing every cluster, stopping at end-of-chain
static void fat16_free_cluster_chain_internal(const fat_node *vol, uint16_t start_cluster) {
    uint16_t current = start_cluster;
    while (current >= 2 && current < FAT16_CLUSTER_BAD) {
        uint16_t next = fat16_get_next_cluster(vol, current);
        fat16_set_next_cluster(vol, current, FAT16_CLUSTER_FREE);
        current = next;
    }
}

// Read a whole cluster (spc sectors) contiguous from its first sector
static uint8_t fat16_read_cluster(const fat_node *vol, uint16_t cluster, uint8_t *buffer) {
    uint32_t sector = fat16_get_cluster_sector(vol, cluster);
    return ((fat_node *)vol)->blockdev.read(&((fat_node *)vol)->blockdev, sector, vol->data.bpb.spc, buffer);
}

// Write a whole cluster (spc sectors) contiguous from its first sector
static uint8_t fat16_write_cluster(const fat_node *vol, uint16_t cluster, const uint8_t *buffer) {
    uint32_t sector = fat16_get_cluster_sector(vol, cluster);
    return ((fat_node *)vol)->blockdev.write(&((fat_node *)vol)->blockdev, sector, vol->data.bpb.spc, (void *)buffer);
}

// Decode the space-padded 8.3 name into "base.ext" (skipping the dot for directories)
static void fat16_read_dirent(struct fat16_dirent *dirent, char *name, size_t name_len) {
    size_t pos = 0;
    for (int i = 0; i < 8 && dirent->name[i] != ' ' && dirent->name[i] != 0; i++) {
        if (pos < name_len - 1) {
            name[pos++] = dirent->name[i];
        }
    }
    
    if (!(dirent->attr & FAT16_ATTR_DIRECTORY)) {
        if (pos < name_len - 1) {
            name[pos++] = '.';
        }
        for (int i = 0; i < 3 && dirent->ext[i] != ' ' && dirent->ext[i] != 0; i++) {
            if (pos < name_len - 1) {
                name[pos++] = dirent->ext[i];
            }
        }
    }
    
    if (pos < name_len) {
        name[pos] = 0;
    }
}

// Fixed root directory size in sectors: rec entries * 32 bytes each
static uint32_t fat16_root_dir_sectors(const fat_node *vol) {
    return (vol->data.bpb.rec * 32 + vol->data.bpb.bps - 1) / vol->data.bpb.bps;
}

// 32-byte dirents per sector
static uint32_t fat16_dir_entries_per_sector(const fat_node *vol) {
    return vol->data.bpb.bps / 32;
}

// 32-byte dirents per whole cluster
static uint32_t fat16_dir_entries_per_cluster(const fat_node *vol) {
    return fat16_dir_entries_per_sector(vol) * vol->data.bpb.spc;
}

// Map directory-entry index |idx| to (sector, offset-within-sector); dir_cluster 0 = fixed root
static uint8_t fat16_dir_entry_location(const fat_node *vol, uint16_t dir_cluster,
                                        uint32_t idx, uint32_t *sector, uint16_t *in_sector) {
    uint32_t eps = fat16_dir_entries_per_sector(vol);
    uint32_t epc = fat16_dir_entries_per_cluster(vol);

    if (dir_cluster == 0) {
        // Root: entries live right after the FATs, before any data cluster
        if (idx / eps >= fat16_root_dir_sectors(vol))
            return 1;
        *sector = vol->data.bpb.rsc + vol->data.bpb.num_fats * vol->data.bpb.fat_s
                + idx / eps;
        *in_sector = idx % eps;
        return 0;
    }

    uint16_t cluster = dir_cluster;
    uint32_t advance = idx / epc;   // how many clusters deep into the chain the dirent sits
    for (uint32_t i = 0; i < advance; i++) {
        uint16_t next = fat16_get_next_cluster(vol, cluster);
        if (next < 2 || next >= FAT16_CLUSTER_BAD)
            return 1;
        cluster = next;
    }

    uint32_t within_cluster = idx % epc;
    *sector = fat16_get_cluster_sector(vol, cluster) + within_cluster / eps;
    *in_sector = within_cluster % eps;   // 32-byte slot offset inside the sector
    return 0;
}

// Read the dir entry at logical index |idx| into |entry|
static uint8_t fat16_read_dirent_index(const fat_node *vol, uint16_t dir_cluster,
                                       uint32_t idx, struct fat16_dirent *entry) {
    uint32_t sector;
    uint16_t in_sector;
    if (fat16_dir_entry_location(vol, dir_cluster, idx, &sector, &in_sector) != 0)
        return 1;

    uint64_t phys = frame_alloc();
    if (!phys)
        return 2;
    uint8_t *buffer = (uint8_t *)phys_to_virt(phys);

    if (((fat_node *)vol)->blockdev.read(&((fat_node *)vol)->blockdev, sector, 1, buffer) != 0) {
        frame_free(phys);
        return 3;
    }

    memcpy(entry, buffer + in_sector * 32, 32);   // 32 bytes per dir entry
    frame_free(phys);
    return 0;
}

// Read-modify-write the dir entry at logical index |idx|
static uint8_t fat16_write_dirent_index(const fat_node *vol, uint16_t dir_cluster,
                                        uint32_t idx, const struct fat16_dirent *entry) {
    uint32_t sector;
    uint16_t in_sector;
    if (fat16_dir_entry_location(vol, dir_cluster, idx, &sector, &in_sector) != 0)
        return 1;

    uint64_t phys = frame_alloc();
    if (!phys)
        return 2;
    uint8_t *buffer = (uint8_t *)phys_to_virt(phys);

    if (((fat_node *)vol)->blockdev.read(&((fat_node *)vol)->blockdev, sector, 1, buffer) != 0) {
        frame_free(phys);
        return 3;
    }

    memcpy(buffer + in_sector * 32, entry, 32);

    if (((fat_node *)vol)->blockdev.write(&((fat_node *)vol)->blockdev, sector, 1, buffer) != 0) {
        frame_free(phys);
        return 4;
    }

    frame_free(phys);
    return 0;
}

// Read a little-endian UCS-2 code unit
static uint16_t fat16_get_ucs2(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

// Write a little-endian UCS-2 code unit
static void fat16_put_ucs2(uint8_t *p, uint16_t c) {
    p[0] = c & 0xFF;
    p[1] = c >> 8;
}

// ASCII case-insensitive filename comparison
static int fat16_name_iequal(const char *a, const char *b) {
    for (;;) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return 0;
        if (ca == 0)
            return 1;
        a++;
        b++;
    }
}

// Standard LFN checksum over the 11 bytes of the 8.3 name, tying LFN slots to their short entry
static uint8_t fat16_lfn_checksum(const uint8_t *short_name) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    return sum;
}

// How many LFN slots a name needs: 13 chars per slot, rounded up
static uint32_t fat16_lfn_count(const char *name) {
    return ((uint32_t)strlen(name) + 12) / 13;
}

// Fold an arbitrary name into an upper-case, space-padded 11-byte short name
static void fat16_short_name(const char *name, uint8_t *short_name) {
    const char *dot = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }

    size_t base_len = dot ? (size_t)(dot - name) : (size_t)strlen(name);
    size_t ext_len = dot ? strlen(dot + 1) : 0;

    memset(short_name, ' ', 11);
    for (size_t i = 0; i < base_len && i < 8; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        short_name[i] = c;
    }
    for (size_t i = 0; i < ext_len && i < 3; i++) {
        char c = dot[i + 1];
        if (c >= 'a' && c <= 'z') c -= 32;
        short_name[8 + i] = c;
    }
}

// Reassemble a long name from |count| LFN slots; builds in reverse (last slot holds chars 0-12)
static size_t fat16_lfn_decode(const struct fat16_dirent *entries, int count,
                               char *out, size_t out_size) {
    size_t pos = 0;
    for (int i = count - 1; i >= 0; i--) {
        const struct fat16_dirent *e = &entries[i];
        const uint8_t *p = e->name;

        uint16_t chars[13];
        // LFN slots smuggle 13 UCS-2 chars across the reserved dirent fields
        chars[0]  = fat16_get_ucs2(p + 1);
        chars[1]  = fat16_get_ucs2(p + 3);
        chars[2]  = fat16_get_ucs2(p + 5);
        chars[3]  = fat16_get_ucs2(&e->name[7]);
        chars[4]  = fat16_get_ucs2(&e->ext[1]);
        chars[5]  = e->crtime;
        chars[6]  = e->crdate;
        chars[7]  = e->ladate;
        chars[8]  = e->cluster_high;
        chars[9]  = e->wtime;
        chars[10] = e->wdate;
        chars[11] = e->size & 0xFFFF;
        chars[12] = e->size >> 16;

        for (int j = 0; j < 13; j++) {
            uint16_t c = chars[j];
            if (c == 0xFFFF || c == 0) {
                // 0xFFFF pads the last LFN slot; 0x0000 also terminates the name
                if (pos < out_size)
                    out[pos] = '\0';
                return pos;
            }
            if (pos + 1 >= out_size)
                return pos;
            out[pos++] = (char)(c & 0x7F);   // input is pure ASCII, so low byte is enough
        }
    }
    if (pos < out_size)
        out[pos] = '\0';
    return pos;
}

// Encode |offset|-indexed slice of |name| (up to 13 chars) into one LFN slot
static void fat16_lfn_encode(struct fat16_dirent *e, uint8_t seq, uint8_t checksum,
                             const char *name, size_t offset) {
    size_t len = strlen(name);
    uint16_t chars[13];

    memset(e, 0, sizeof(*e));
    for (int i = 0; i < 13; i++) {
        size_t ci = offset + (size_t)i;
        chars[i] = (ci < len) ? (uint16_t)(uint8_t)name[ci] : 0xFFFF;   // 0xFFFF pads out-of-range chars
    }

    e->name[0] = seq;
    fat16_put_ucs2(e->name + 1, chars[0]);
    fat16_put_ucs2(e->name + 3, chars[1]);
    fat16_put_ucs2(e->name + 5, chars[2]);
    fat16_put_ucs2(&e->name[7], chars[3]);
    fat16_put_ucs2(&e->ext[1], chars[4]);
    e->attr = FAT16_ATTR_LFN;
    e->reserved = 0;
    e->crtime_tenths = checksum;
    e->crtime = chars[5];
    e->crdate = chars[6];
    e->ladate = chars[7];
    e->cluster_high = chars[8];
    e->wtime = chars[9];
    e->wdate = chars[10];
    e->cluster_low = 0;
    e->size = (chars[11] & 0xFFFF) | ((uint32_t)(chars[12] & 0xFFFF) << 16);
}

// Locate the dirent for |name|, skipping deleted slots and following LFN runs
static struct fat16_dirent *fat16_find_dirent(const fat_node *vol, uint16_t dir_cluster, const char *name, struct fat16_dirent *result) {
    for (uint32_t idx = 0; idx < 65536; idx++) {
        struct fat16_dirent entry;
        if (fat16_read_dirent_index(vol, dir_cluster, idx, &entry) != 0)
            return NULL;
        if (entry.name[0] == 0)
            return NULL;   // a full zero name marks the end of the directory

        if (entry.attr == FAT16_ATTR_LFN) {
            // Collect the run of LFN slots, then decode and match the long name
            struct fat16_dirent lfn_entries[21];
            int count = 0;
            uint32_t k = idx;
            while (count < 21) {
                struct fat16_dirent e;
                if (fat16_read_dirent_index(vol, dir_cluster, k, &e) != 0)
                    break;
                if (e.attr != FAT16_ATTR_LFN)
                    break;
                lfn_entries[count++] = e;
                k++;
            }

            char long_name[256];
            size_t long_len = fat16_lfn_decode(lfn_entries, count, long_name, sizeof(long_name));
            if (long_len > 0 && strcmp(long_name, name) == 0) {
                struct fat16_dirent short_entry;
                if (fat16_read_dirent_index(vol, dir_cluster, k, &short_entry) != 0)
                    return NULL;
                memcpy(result, &short_entry, sizeof(struct fat16_dirent));
                return result;
            }
            idx = k;
            continue;
        }

        if (entry.name[0] == 0xE5)
            continue;   // 0xE5 = deleted entry, skip it

        char entry_name[256];
        fat16_read_dirent(&entry, entry_name, sizeof(entry_name));

        if (fat16_name_iequal(entry_name, name)) {
            memcpy(result, &entry, sizeof(struct fat16_dirent));
            return result;
        }
    }

    return NULL;
}

// Walk the cluster chain of |start_cluster|, copying |size| bytes to |buffer|
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

// Write |size| bytes by allocating a fresh cluster chain; returns the chain head in *start_cluster
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
        
        uint8_t *cluster_buf = (uint8_t *)phys_to_virt(phys);
        memset(cluster_buf, 0, cluster_bytes);      // zero-fill the whole cluster first
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

// Return the logical index of the dirent for |name| (past any LFN slots)
static uint8_t fat16_find_entry_index(const fat_node *vol, uint16_t dir_cluster, const char *name, uint32_t *index) {
    for (uint32_t idx = 0; idx < 65536; idx++) {
        struct fat16_dirent entry;
        if (fat16_read_dirent_index(vol, dir_cluster, idx, &entry) != 0)
            return 1;
        if (entry.name[0] == 0)
            return 1;

        if (entry.attr == FAT16_ATTR_LFN) {
            struct fat16_dirent lfn_entries[21];
            int count = 0;
            uint32_t k = idx;
            while (count < 21) {
                struct fat16_dirent e;
                if (fat16_read_dirent_index(vol, dir_cluster, k, &e) != 0)
                    break;
                if (e.attr != FAT16_ATTR_LFN)
                    break;
                lfn_entries[count++] = e;
                k++;
            }

            char long_name[256];
            size_t long_len = fat16_lfn_decode(lfn_entries, count, long_name, sizeof(long_name));
            if (long_len > 0 && fat16_name_iequal(long_name, name)) {
                *index = k;
                return 0;
            }
            idx = k;
            continue;
        }

        if (entry.name[0] == 0xE5)
            continue;

        char entry_name[256];
        fat16_read_dirent(&entry, entry_name, sizeof(entry_name));

        if (fat16_name_iequal(entry_name, name)) {
            *index = idx;
            return 0;
        }
    }

    return 1;
}

// Create a directory entry: find a free slot run, then write LFN slots followed by the 8.3 entry
static uint8_t fat16_create_dirent(const fat_node *vol, uint16_t dir_cluster, const char *name, uint8_t attr, uint16_t start_cluster, uint32_t size) {
    uint8_t short_name[11];
    fat16_short_name(name, short_name);
    uint32_t n_lfn = fat16_lfn_count(name);
    uint32_t need = n_lfn + 1;

    for (uint32_t idx = 0; idx < 65536;) {
        struct fat16_dirent entry;
        int read = fat16_read_dirent_index(vol, dir_cluster, idx, &entry);
        if (read != 0) {
            // Walked off the cluster chain: grow the directory with a fresh zeroed cluster
            if (dir_cluster == 0)
                return 7;   // the fixed root directory cannot grow
            uint16_t last = dir_cluster;
            uint16_t nxt;
            while ((nxt = fat16_get_next_cluster(vol, last)) >= 2 && nxt < FAT16_CLUSTER_BAD)
                last = nxt;
            uint16_t new_cluster = fat16_allocate_cluster(vol);
            if (!new_cluster)
                return 8;
            if (fat16_set_next_cluster(vol, last, new_cluster) != 0) {
                fat16_free_cluster_chain(vol, new_cluster);
                return 9;
            }
            uint64_t zphys = frame_alloc();
            if (!zphys) {
                fat16_free_cluster_chain(vol, new_cluster);
                return 12;
            }
            uint8_t *zbuf = (uint8_t *)phys_to_virt(zphys);
            memset(zbuf, 0, vol->data.bpb.spc * vol->data.bpb.bps);
            fat16_write_cluster(vol, new_cluster, zbuf);
            frame_free(zphys);
        } else if (entry.name[0] != 0 && entry.name[0] != 0xE5) {
            idx++;
            continue;
        }

        uint32_t start = idx;
        uint32_t free_run = 0;
        uint32_t k = idx;
        while (k < 65536 && free_run < need) {
            struct fat16_dirent e2;
            if (fat16_read_dirent_index(vol, dir_cluster, k, &e2) != 0)
                break;
            if (e2.name[0] != 0 && e2.name[0] != 0xE5)
                break;
            free_run++;
            k++;
        }

        if (free_run >= need) {
            uint8_t checksum = fat16_lfn_checksum(short_name);
            for (uint32_t i = 0; i < n_lfn; i++) {
                struct fat16_dirent lfn;
                // Slots are numbered high-to-low; bit 0x40 marks the final (first-read) slot
                fat16_lfn_encode(&lfn, (uint8_t)((n_lfn - i) | ((i == 0) ? 0x40 : 0)), checksum, name, i * 13);
                if (fat16_write_dirent_index(vol, dir_cluster, start + i, &lfn) != 0)
                    return 6;
            }

            struct fat16_dirent new_entry;
            memset(&new_entry, 0, sizeof(new_entry));
            memcpy(new_entry.name, short_name, 11);
            new_entry.attr = attr;
            new_entry.cluster_low = start_cluster & 0xFFFF;
            new_entry.cluster_high = (start_cluster >> 16) & 0xFFFF;
            new_entry.size = size;
            return (fat16_write_dirent_index(vol, dir_cluster, start + n_lfn, &new_entry) == 0) ? 0 : 6;
        }

        idx = k;
    }

    return 11;
}

// Mark a dirent (and its LFN slots) as deleted with 0xE5; frees the file's cluster chain
static uint8_t fat16_delete_dirent(const fat_node *vol, uint16_t dir_cluster, const char *name) {
    uint32_t index;
    if (fat16_find_entry_index(vol, dir_cluster, name, &index) != 0)
        return 5;

    struct fat16_dirent entry;
    if (fat16_read_dirent_index(vol, dir_cluster, index, &entry) != 0)
        return 5;

    uint16_t cluster = entry.cluster_low | (entry.cluster_high << 16);
    if (!(entry.attr & FAT16_ATTR_DIRECTORY) && cluster)
        fat16_free_cluster_chain(vol, cluster);

    entry.name[0] = 0xE5;
    if (fat16_write_dirent_index(vol, dir_cluster, index, &entry) != 0)
        return 4;

    uint32_t k = index;
    while (k > 0) {
        // Delete the LFN slots that run backwards from the entry too
        struct fat16_dirent e;
        if (fat16_read_dirent_index(vol, dir_cluster, k - 1, &e) != 0)
            break;
        if (e.attr != FAT16_ATTR_LFN)
            break;
        e.name[0] = 0xE5;
        if (fat16_write_dirent_index(vol, dir_cluster, k - 1, &e) != 0)
            break;
        k--;
    }

    return 0;
}

// Dump the parsed BPB/EBR to the kernel console
static void fat16_debug_print(const fat *f) {
    char oem[9]  = {0};
    char lab[12] = {0};
    char type[9] = {0};

    memcpy(oem,  f->bpb.oem_name, 8);
    memcpy(lab,  f->ebr.vol_lab,  11);
    memcpy(type, f->ebr.f_type,   8);

    print("FAT16 BPB: oem='%s' bps=%u spc=%u rsc=%u fats=%u root=%u ts16=%u media=0x%02x fat_s=%u hidden=%u ts32=%u",
             oem, f->bpb.bps, f->bpb.spc, f->bpb.rsc, f->bpb.num_fats,
             f->bpb.rec, f->bpb.ts16, f->bpb.media, f->bpb.fat_s,
             f->bpb.hidden, f->bpb.ts32);

    print("FAT16 EBR: sig=0x%02x vol_id=0x%08x label='%s' type='%s'",
             f->ebr.bsig, f->ebr.vol_id, lab, type);
}

// Read the boot sector and unpack it into a fat volume struct
static fat *read_fat(vfs_blockdev_t *dev) {
    uint64_t phys = frame_alloc();
    if (!phys) {
        return NULL;
    }

    void *sector = (void *)phys_to_virt(phys);

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

// Iterate fat_size until the cluster count lands in the FAT16 window (4085..65524)
static uint16_t fat16_compute_fat_size(uint32_t total_sectors) {
    uint16_t root_dir_sectors = (FAT16_ROOT_ENTRIES * 32 + FAT16_SECTOR_SIZE - 1) / FAT16_SECTOR_SIZE;
    uint16_t fat_size = 1;
    for (;;) {
        uint32_t data_sectors = total_sectors - FAT16_RESERVED_SECTORS - FAT16_NUM_FATS * fat_size - root_dir_sectors;
        if ((int32_t)data_sectors <= 0) {
            return 0;
        }
        uint32_t cluster_count = data_sectors / FAT16_CLUSTER_SIZE;
        if (cluster_count < 4085 || cluster_count >= 65525) {
            return 0;
        }
        uint16_t next = (uint16_t)((cluster_count * 2 + FAT16_SECTOR_SIZE - 1) / FAT16_SECTOR_SIZE);
        if (next == fat_size) {
            return fat_size;
        }
        fat_size = next;
    }
}

// Basic sanity checks on a parsed boot sector/extended BPB
static uint8_t fat16_validate(const fat *f) {
    const struct bpb *b = &f->bpb;
    if (b->bps < 512 || b->bps > 4096 || (b->bps & (b->bps - 1))) {
        return 1;
    }
    if (!b->spc || (b->spc & (b->spc - 1))) {
        return 2;
    }
    if (b->rsc < 1) {
        return 3;
    }
    if (b->num_fats < 1) {
        return 4;
    }
    if (b->fat_s == 0) {
        return 5;
    }
    if (b->ts16 == 0 && b->ts32 == 0) {
        return 6;
    }
    if (f->ebr.bsig != 0x29) {
        return 7;
    }
    if (f->ebr.vol_id == 0) {
        return 8;
    }
    return 0;
}

// Wipe a drive and write a fresh FAT16 boot sector, FATs, and empty root directory
uint8_t fat16_format(vfs_blockdev_t *dev, uint32_t total_sectors) {
    uint16_t fat_size = fat16_compute_fat_size(total_sectors);
    if (!fat_size) {
        return 1;
    }

    uint64_t phys = frame_alloc();
    if (!phys) {
        return 2;
    }

    uint8_t *sector = (uint8_t *)phys_to_virt(phys);
    memset(sector, 0, FAT16_SECTOR_SIZE);

    // Jump boot instruction + OEM name + BPB fields, byte-by-byte
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
        // 16-bit sector count; larger volumes use the 32-bit field instead
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
    sector[36] = 0x80;   // drive number (0x80 = first hard disk)
    sector[37] = 0;
    sector[38] = 0x29;   // extended boot signature present
    uint32_t vol_id = 0x12345678;
    sector[39] = vol_id & 0xFF;
    sector[40] = (vol_id >> 8)  & 0xFF;
    sector[41] = (vol_id >> 16) & 0xFF;
    sector[42] = (vol_id >> 24) & 0xFF;
    memcpy(sector + 43, FAT16_VOL_LABEL, 11);
    memcpy(sector + 54, FAT16_FS_TYPE,   8);
    sector[510] = 0x55;   // boot record signature 0x55AA
    sector[511] = 0xAA;

    if (dev->write(dev, FAT16_LBA, 1, sector) != 0) {
        frame_free(phys);
        return 3;
    }

    memset(sector, 0, FAT16_SECTOR_SIZE);
    // FAT entry 0 holds the media byte; entries 1 and 2 mark a reserved and the root's EOF
    sector[0] = FAT16_MEDIA;
    sector[1] = 0xFF;
    sector[2] = 0xFF;
    sector[3] = 0xFF;

    for (uint8_t i = 0; i < FAT16_NUM_FATS; i++) {
        // First FAT sector has the media/reserved entries; the rest are zeroed
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
    uint32_t root_base = FAT16_RESERVED_SECTORS + FAT16_NUM_FATS * fat_size;   // root sits right after the FATs
    for (uint16_t i = 0; i < root_dir_sectors; i++) {
        if (dev->write(dev, root_base + i, 1, sector) != 0) {
            frame_free(phys);
            return 6;
        }
    }

    frame_free(phys);
    return 0;
}

// Raw passthrough read of |count| sectors starting at |lba|
uint8_t fat16_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buffer) {
    return dev->read(dev, lba, count, buffer);
}

// Probe |dev| as a FAT16 volume: read the boot sector, validate it, and register the volume
void *fat16_init(vfs_blockdev_t *dev) {
    if (!fat16_volumes_ready) {
        fat_list_init(&fat16_volumes);
        fat16_volumes_ready = 1;
    }

    fat *f = read_fat(dev);
    if (!f) {
        return NULL;
    }

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

// Public wrapper: read |size| bytes from a file's cluster chain
uint8_t fat16_read_file(const void *vol_ptr, uint16_t start_cluster, uint32_t size, uint8_t *buffer, uint32_t *bytes_read) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) {
        return 1;
    }
    return fat16_read_file_core(vol, start_cluster, size, buffer, bytes_read);
}

// Public wrapper: write |size| bytes to a fresh cluster chain
uint8_t fat16_write_file(const void *vol_ptr, uint16_t *start_cluster, const uint8_t *buffer, uint32_t size, uint32_t *bytes_written) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) {
        return 1;
    }
    return fat16_write_file_core(vol, start_cluster, buffer, size, bytes_written);
}

// Create a file: write its data out, then add a dirent pointing at the new chain
uint8_t fat16_create_file(const void *vol_ptr, uint16_t dir_cluster, const char *name, const uint8_t *buffer, uint32_t size) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) {
        return 1;
    }
    
    uint16_t start_cluster = 0;
    uint32_t bytes_written = 0;
    
    if (size > 0) {
        if (fat16_write_file_core(vol, &start_cluster, buffer, size, &bytes_written) != 0) {
            return 2;
        }
    }
    
    if (fat16_create_dirent(vol, dir_cluster, name, FAT16_ATTR_ARCHIVE, start_cluster, size) != 0) {
        if (start_cluster) {
            fat16_free_cluster_chain_internal(vol, start_cluster);
        }
        return 3;
    }
    
    return 0;
}

// Public wrapper: free every cluster in |start_cluster|'s chain
void fat16_free_cluster_chain(const void *vol_ptr, uint16_t start_cluster) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol)
        return;
    fat16_free_cluster_chain_internal(vol, start_cluster);
}

// Delete a file's dirent and free its clusters
uint8_t fat16_delete_file(const void *vol_ptr, uint16_t dir_cluster, const char *name) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) {
        return 1;
    }
    
    return fat16_delete_dirent(vol, dir_cluster, name);
}

// Create an empty subdirectory: allocate a cluster and add a dirent for it
uint8_t fat16_create_directory(const void *vol_ptr, uint16_t parent_cluster, const char *name) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) {
        return 1;
    }
    
    uint16_t new_cluster = fat16_allocate_cluster(vol);
    if (!new_cluster) {
        return 2;
    }
    
    uint64_t phys = frame_alloc();
    if (!phys) {
        fat16_free_cluster_chain(vol, new_cluster);
        return 3;
    }
    
    uint8_t *buffer = (uint8_t *)phys_to_virt(phys);
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

// Remove a directory only if it holds no real entries (free slots and LFN slots don't count)
uint8_t fat16_delete_directory(const void *vol_ptr, uint16_t parent_cluster, const char *name) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) {
        return 1;
    }
    
    struct fat16_dirent dirent;
    if (!fat16_find_dirent(vol, parent_cluster, name, &dirent)) {
        return 2;
    }
    
    if (!(dirent.attr & FAT16_ATTR_DIRECTORY)) {
        return 3;
    }
    
    uint16_t dir_cluster = dirent.cluster_low | (dirent.cluster_high << 16);
    
    uint64_t phys = frame_alloc();
    if (!phys) {
        return 4;
    }
    
    uint8_t *buffer = (uint8_t *)phys_to_virt(phys);
    uint16_t current = dir_cluster;
    uint8_t is_empty = 1;
    
    while (current >= 2 && current < FAT16_CLUSTER_BAD) {
        if (fat16_read_cluster(vol, current, buffer) != 0) {
            frame_free(phys);
            return 5;
        }
        
        for (int i = 0; i < vol->data.bpb.bps / 32; i++) {
            struct fat16_dirent *entry = (struct fat16_dirent *)(buffer + i * 32);
            
            if (entry->name[0] == 0 || entry->name[0] == 0xE5) {
                continue;
            }
            if (entry->attr == FAT16_ATTR_LFN) {
                continue;
            }
            
            is_empty = 0;
            break;
        }
        
        if (!is_empty) {
            break;
        }
        current = fat16_get_next_cluster(vol, current);
    }
    
    frame_free(phys);
    
    if (!is_empty) {
        return 6;
    }
    
    fat16_free_cluster_chain(vol, dir_cluster);
    
    return fat16_delete_dirent(vol, parent_cluster, name);
}

// Fill |entries| (up to *count) with the directory's name/attr/cluster listing
uint8_t fat16_list_directory(const void *vol_ptr, uint16_t dir_cluster, fat16_dir_entry *entries, uint16_t *count) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) {
        return 1;
    }

    uint16_t index = 0;
    uint16_t max_count = *count;

    for (uint32_t idx = 0; idx < 65536; idx++) {
        struct fat16_dirent entry;
        if (fat16_read_dirent_index(vol, dir_cluster, idx, &entry) != 0)
            break;
        if (entry.name[0] == 0)
            break;

        if (entry.attr == FAT16_ATTR_LFN) {
            struct fat16_dirent lfn_entries[21];
            int lfn_count = 0;
            uint32_t k = idx;
            while (lfn_count < 21) {
                struct fat16_dirent e;
                if (fat16_read_dirent_index(vol, dir_cluster, k, &e) != 0)
                    break;
                if (e.attr != FAT16_ATTR_LFN)
                    break;
                lfn_entries[lfn_count++] = e;
                k++;
            }

            struct fat16_dirent short_entry;
            if (fat16_read_dirent_index(vol, dir_cluster, k, &short_entry) != 0)
                break;

            char long_name[256];
            size_t long_len = fat16_lfn_decode(lfn_entries, lfn_count, long_name, sizeof(long_name));
            if (long_len > 0) {
                entries[index].start_cluster = short_entry.cluster_low | (short_entry.cluster_high << 16);
                entries[index].attr = short_entry.attr;
                strncpy(entries[index].name, long_name, sizeof(entries[index].name) - 1);
                entries[index].name[sizeof(entries[index].name) - 1] = '\0';
                index++;
                if (index >= max_count)
                    break;
            }
            idx = k;
            continue;
        }

        if (entry.name[0] == 0xE5)
            continue;

        fat16_read_dirent(&entry, entries[index].name, sizeof(entries[index].name));
        entries[index].start_cluster = entry.cluster_low | (entry.cluster_high << 16);
        entries[index].attr = entry.attr;
        index++;

        if (index >= max_count)
            break;
    }

    *count = index;
    return 0;
}

// Look up one entry and return its cluster, size, attrs, and 8.3 name
uint8_t fat16_find_file(const void *vol_ptr, uint16_t dir_cluster, const char *name, fat16_file_handle *handle) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) {
        return 1;
    }
    if (!handle) {
        return 2;
    }
    
    struct fat16_dirent dirent;
    if (!fat16_find_dirent(vol, dir_cluster, name, &dirent)) {
        return 3;
    }
    
    handle->start_cluster = dirent.cluster_low | (dirent.cluster_high << 16);
    handle->size = dirent.size;
    handle->attr = dirent.attr;
    fat16_read_dirent(&dirent, handle->name, sizeof(handle->name));
    
    return 0;
}

// Rewrite the stored cluster/size of an existing dirent (used to grow a file)
uint8_t fat16_create_dirent_update(const void *vol_ptr, uint16_t dir_cluster,
                                   const char *name, uint16_t start_cluster,
                                   uint32_t size) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) return 1;

    uint32_t index;
    if (fat16_find_entry_index(vol, dir_cluster, name, &index) != 0)
        return 4;

    struct fat16_dirent entry;
    if (fat16_read_dirent_index(vol, dir_cluster, index, &entry) != 0)
        return 4;

    entry.cluster_low  = start_cluster & 0xFFFF;
    entry.cluster_high = (start_cluster >> 16) & 0xFFFF;
    entry.size         = size;

    return (fat16_write_dirent_index(vol, dir_cluster, index, &entry) == 0) ? 0 : 5;
}

// Return the serial number this volume was formatted with
uint32_t fat16_get_volume_id(const void *vol_ptr) {
    const fat_node *vol = (const fat_node *)vol_ptr;
    if (!vol) {
        return 0;
    }
    return vol->data.ebr.vol_id;
}