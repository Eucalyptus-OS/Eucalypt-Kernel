#include <stdint.h>
#include <drivers/hardware_devices.h>
#include <mm/heap.h>
#include <logging/printk.h>
#include <mem.h>
#include <drivers/fs/gpt.h>

#define GPT_MAGIC        "\x45\x46\x49\x20\x50\x41\x52\x54"
#define GPT_MAGIC_LEN    8
#define PMBR_GPT_OS_TYPE 0xEE
#define PMBR_SIGNATURE   0xAA55

struct __attribute__((packed)) pmbr {
    uint8_t  bootstrap[446];
    uint8_t  boot_indicator;
    uint8_t  starting_chs[3];
    uint8_t  os_type;
    uint8_t  ending_chs[3];
    uint32_t starting_lba;
    uint32_t partition_size;
    uint8_t  other_partitions[48];
    uint16_t signature;
};

struct __attribute__((packed)) pth {
    uint8_t  signature[8];
    uint32_t gpt_rev;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
    uint64_t lba;
    uint64_t lba2;
    uint64_t last_lba;
    uint8_t  g_uid[16];
    uint64_t partition_entry_lba;
    uint32_t partition_entries;
    uint32_t part_size;
    uint32_t crc32_entry;
    uint32_t reserved2;
};

struct gpt {
    struct pmbr            *pmbr;
    struct pth             *pth;
    struct partition_entry *partition_entries;
};

static void gpt_free(struct gpt *gpt) {
    if (!gpt) return;
    if (gpt->pmbr)              kfree(gpt->pmbr);
    if (gpt->pth)               kfree(gpt->pth);
    if (gpt->partition_entries) kfree(gpt->partition_entries);
    kfree(gpt);
}

static uint8_t dev_read(storage_device_t *dev, uint64_t sector, uint32_t count, void *buf) {
    switch (dev->type) {
        case STORAGE_DEVICE_AHCI:
            return dev->read(dev->sata_controller_id, dev->sata_port_id, sector, count, buf);
        case STORAGE_DEVICE_IDE:
            return dev->read(0, dev->ide_drive_num, sector, count, buf);
        case STORAGE_DEVICE_NVME:
            return dev->read(dev->sata_controller_id, dev->sata_port_id, sector, count, buf);
        default:
            return 1;
    }
}

static uint8_t dev_write(storage_device_t *dev, uint64_t sector, uint32_t count, const void *buf) {
    switch (dev->type) {
        case STORAGE_DEVICE_AHCI:
            return dev->write(dev->sata_controller_id, dev->sata_port_id, sector, count, buf);
        case STORAGE_DEVICE_IDE:
            return dev->write(0, dev->ide_drive_num, sector, count, buf);
        case STORAGE_DEVICE_NVME:
            return dev->write(dev->sata_controller_id, dev->sata_port_id, sector, count, buf);
        default:
            return 1;
    }
}

static uint8_t gpt_read(storage_device_t *dev, struct gpt **out) {
    struct gpt *gpt = kmalloc(sizeof(struct gpt));
    if (!gpt) return 1;

    gpt->pmbr              = NULL;
    gpt->pth               = NULL;
    gpt->partition_entries = NULL;

    gpt->pmbr = kmalloc(sizeof(struct pmbr));
    if (!gpt->pmbr) goto fail;

    if (dev_read(dev, 0, 1, gpt->pmbr) != 0)                          goto fail;
    if (gpt->pmbr->signature != PMBR_SIGNATURE)                       goto fail;
    if (gpt->pmbr->os_type   != PMBR_GPT_OS_TYPE)                     goto fail;

    gpt->pth = kmalloc(sizeof(struct pth));
    if (!gpt->pth) goto fail;

    if (dev_read(dev, 1, 1, gpt->pth) != 0)                           goto fail;
    if (memcmp(gpt->pth->signature, GPT_MAGIC, GPT_MAGIC_LEN) != 0)   goto fail;
    if (gpt->pth->partition_entries == 0 || gpt->pth->part_size == 0) goto fail;

    uint32_t entries_size   = gpt->pth->partition_entries * gpt->pth->part_size;
    uint32_t sectors_needed = (entries_size + 511) / 512;

    gpt->partition_entries = kmalloc(entries_size);
    if (!gpt->partition_entries) goto fail;

    if (dev_read(dev, gpt->pth->partition_entry_lba, sectors_needed, gpt->partition_entries) != 0)
        goto fail;

    *out = gpt;
    return 0;

fail:
    gpt_free(gpt);
    return 1;
}

uint8_t gpt_write_partition(storage_device_t *dev, uint32_t slot, struct partition_entry *entry) {
    if (!dev || !entry) return 1;

    struct gpt *gpt = NULL;
    uint8_t     ret = 1;

    if (gpt_read(dev, &gpt) != 0) {
        log_warn("gpt_write_partition: failed to read GPT\n");
        return 1;
    }

    if (slot >= gpt->pth->partition_entries) {
        log_warn("gpt_write_partition: slot %u out of range (%u entries)\n", slot, gpt->pth->partition_entries);
        goto cleanup;
    }

    struct partition_entry *target =
        (struct partition_entry *)((uint8_t *)gpt->partition_entries + slot * gpt->pth->part_size);
    memcpy(target, entry, sizeof(struct partition_entry));

    uint32_t entries_size   = gpt->pth->partition_entries * gpt->pth->part_size;
    uint32_t sectors_needed = (entries_size + 511) / 512;

    if (dev_write(dev, gpt->pth->partition_entry_lba, sectors_needed, gpt->partition_entries) != 0) {
        log_warn("gpt_write_partition: failed to write primary partition entries\n");
        goto cleanup;
    }

    if (dev_write(dev, gpt->pth->lba2, sectors_needed, gpt->partition_entries) != 0) {
        log_warn("gpt_write_partition: failed to write backup partition entries\n");
        goto cleanup;
    }

    log_info("gpt_write_partition: wrote slot %u (LBA %llu-%llu)\n", slot, entry->lba, entry->last_lba);
    ret = 0;

cleanup:
    gpt_free(gpt);
    return ret;
}

static uint8_t gpt_parse_device(storage_device_t *dev, uint8_t dev_index) {
    struct gpt *gpt = NULL;

    if (gpt_read(dev, &gpt) != 0) {
        log_warn("Device %u: failed to read GPT\n", dev_index);
        return 1;
    }

    log_info("Device %u: GPT rev 0x%08X, %u entries (%u bytes each)\n",
             dev_index, gpt->pth->gpt_rev, gpt->pth->partition_entries, gpt->pth->part_size);

    static const uint8_t null_guid[16] = {0};

    for (uint32_t p = 0; p < gpt->pth->partition_entries; p++) {
        struct partition_entry *e =
            (struct partition_entry *)((uint8_t *)gpt->partition_entries + p * gpt->pth->part_size);
        if (memcmp(e->type_guid, null_guid, 16) == 0)
            continue;
        log_info("Device %u: partition %u  LBA %llu-%llu\n", dev_index, p, e->lba, e->last_lba);
    }

    gpt_free(gpt);
    return 0;
}

uint8_t gpt_init() {
    uint8_t total = get_total_drive_count();

    if (total == 0) {
        log_warn("gpt_init: no storage devices found\n");
        return 1;
    }

    for (uint8_t i = 0; i < total; i++) {
        storage_device_t *dev = get_storage_device_by_index(i);
        if (!dev) {
            log_warn("gpt_init: failed to get device %u\n", i);
            continue;
        }
        if (gpt_parse_device(dev, i) != 0)
            log_warn("gpt_init: device %u is not GPT or failed to parse\n", i);
    }

    return 0;
}