#include <stdint.h>
#include <mem.h>
#include <mm/heap.h>
#include <drivers/block/ahci.h>
#include <drivers/block/nvme.h>
#include <drivers/block/ide.h>
#include <drivers/fs/vfs/blockdev.h>
#include <drivers/fs/fat16/fat16.h>
#include <drivers/fs/vfs/vfs.h>

#define MAX_DRIVES 254

static vfs_mount_t mount_table[MAX_DRIVES];
static uint8_t     mount_count = 0;
static uint8_t     vfs_ready   = 0;

static int find_letter_slot(char letter) {
    for (int i = 0; i < mount_count; i++)
        if (mount_table[i].letter == letter) return i;
    return -1;
}

vfs_mount_t *vfs_get_mount(char letter) {
    int slot = find_letter_slot(letter);
    if (slot < 0) return 0;
    return &mount_table[slot];
}

static uint8_t ahci_blockdev_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return ahci_read(priv->addr.ahci.controller, priv->addr.ahci.port, lba, count, buf);
}

static uint8_t ahci_blockdev_write(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, const void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return ahci_write(priv->addr.ahci.controller, priv->addr.ahci.port, lba, count, buf);
}

static uint8_t nvme_blockdev_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return nvme_read(priv->addr.nvme.controller, priv->addr.nvme.nsid, lba, count, buf);
}

static uint8_t nvme_blockdev_write(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, const void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return nvme_write(priv->addr.nvme.controller, priv->addr.nvme.nsid, lba, count, buf);
}

static uint8_t ide_blockdev_read(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return ide_read(priv->addr.ide.bus, priv->addr.ide.drive, lba, count, buf);
}

static uint8_t ide_blockdev_write(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, const void *buf) {
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)dev->priv;
    return ide_write(priv->addr.ide.bus, priv->addr.ide.drive, lba, count, buf);
}

fs_t vfs_get_type(vfs_blockdev_t *blockdev) {
    uint8_t buf[512];
    if (blockdev->read(blockdev, 0, 1, buf) != 0)
        return -1;

    if (memcmp(buf + 0x36, "FAT12   ", 8) == 0)
        return fat12;

    if (memcmp(buf + 0x36, "FAT16   ", 8) == 0)
        return fat16;

    if (memcmp(buf + 0x52, "FAT32   ", 8) == 0)
        return fat32;

    if (memcmp(buf + 0x03, "EXFAT   ", 8) == 0)
        return exfat;

    return -1;
}

uint8_t vfs_mount(enum StorageDevType dev, char letter, vfs_dev_addr_t addr) {
    if (find_letter_slot(letter) >= 0)
        return VFS_ERR_LETTER_IN_USE;

    if (mount_count >= MAX_DRIVES)
        return VFS_ERR_NO_SLOTS;

    vfs_blockdev_priv_t *priv = kmalloc(sizeof(vfs_blockdev_priv_t));
    if (!priv)
        return VFS_ERR_NO_SLOTS;

    priv->addr = addr;

    vfs_blockdev_t blockdev = {0};
    blockdev.priv = priv;

    if (dev == AHCI) {
        uint8_t controller = addr.ahci.controller;
        uint8_t port       = addr.ahci.port;

        if (controller >= ahci_get_controller_count()) { kfree(priv); return VFS_ERR_INVALID_DEV; }

        ahci_controller_t *c = ahci_get_controller(controller);
        if (!c)                                  { kfree(priv); return VFS_ERR_INVALID_DEV; }
        if (!c->ports[port].present)             { kfree(priv); return VFS_ERR_INVALID_DEV; }
        if (c->ports[port].assigned_letter != 0) { kfree(priv); return VFS_ERR_ALREADY_MOUNTED; }

        blockdev.read  = ahci_blockdev_read;
        blockdev.write = ahci_blockdev_write;

    } else if (dev == NVME) {
        uint8_t controller = addr.nvme.controller;
        uint32_t nsid      = addr.nvme.nsid;

        if (controller >= nvme_get_controller_count()) { kfree(priv); return VFS_ERR_INVALID_DEV; }

        nvme_controller_t *c = nvme_get_controller(controller);
        if (!c)                                            { kfree(priv); return VFS_ERR_INVALID_DEV; }
        if (!nvme_namespace_present(c, nsid))       { kfree(priv); return VFS_ERR_INVALID_DEV; }
        if (nvme_namespace_letter(c, nsid) != 0)    { kfree(priv); return VFS_ERR_ALREADY_MOUNTED; }

        blockdev.read  = nvme_blockdev_read;
        blockdev.write = nvme_blockdev_write;

    } else if (dev == IDE) {
        uint8_t bus   = addr.ide.bus;
        uint8_t drive = addr.ide.drive;

        if (bus > 1 || drive > 1)              { kfree(priv); return VFS_ERR_INVALID_DEV; }
        if (!ide_drive_present(bus, drive))    { kfree(priv); return VFS_ERR_INVALID_DEV; }
        if (ide_drive_letter(bus, drive) != 0) { kfree(priv); return VFS_ERR_ALREADY_MOUNTED; }

        blockdev.read  = ide_blockdev_read;
        blockdev.write = ide_blockdev_write;

    } else {
        kfree(priv);
        return VFS_ERR_UNKNOWN_DEV;
    }

    mount_table[mount_count].letter   = letter;
    mount_table[mount_count].blockdev = blockdev;
    mount_table[mount_count].dev      = dev;
    mount_table[mount_count].addr     = addr;
    mount_count++;

    fs_t type = vfs_get_type(&blockdev);
    if (type != fat16) {
        mount_count--;
        kfree(priv);
        return VFS_ERR_FS_INIT;
    }

    void *vol_ptr = fat16_init(&blockdev);
    if (!vol_ptr) {
        mount_count--;
        kfree(priv);
        return VFS_ERR_FS_INIT;
    }

    mount_table[mount_count - 1].priv = vol_ptr;

    if (dev == AHCI) {
        ahci_controller_t *c = ahci_get_controller(addr.ahci.controller);
        if (c) c->ports[addr.ahci.port].assigned_letter = letter;
    } else if (dev == NVME) {
        nvme_controller_t *c = nvme_get_controller(addr.nvme.controller);
        if (c) nvme_set_namespace_letter(c, addr.nvme.nsid, letter);
    } else if (dev == IDE) {
        ide_set_drive_letter(addr.ide.bus, addr.ide.drive, letter);
    }

    return VFS_OK;
}

void vfs_unmount(char letter) {
    int slot = find_letter_slot(letter);
    if (slot < 0) return;

    vfs_mount_t *m = &mount_table[slot];
    vfs_blockdev_priv_t *priv = (vfs_blockdev_priv_t *)m->blockdev.priv;

    if (m->dev == AHCI) {
        ahci_controller_t *c = ahci_get_controller(m->addr.ahci.controller);
        if (c) c->ports[m->addr.ahci.port].assigned_letter = 0;
    } else if (m->dev == NVME) {
        nvme_controller_t *c = nvme_get_controller(m->addr.nvme.controller);
        if (c) nvme_set_namespace_letter(c, m->addr.nvme.nsid, 0);
    } else if (m->dev == IDE) {
        ide_set_drive_letter(m->addr.ide.bus, m->addr.ide.drive, 0);
    }

    kfree(priv);

    for (int i = slot; i < mount_count - 1; i++)
        mount_table[i] = mount_table[i + 1];
    mount_count--;
}

uint8_t vfs_init() {
    if (vfs_ready) return 0;

    mount_count = 0;
    vfs_ready   = 1;

    char letter = 'C';
    uint8_t ctrl_count = ahci_get_controller_count();

    for (uint8_t c = 0; c < ctrl_count; c++) {
        ahci_controller_t *ctrl = ahci_get_controller(c);
        if (!ctrl) continue;

        for (uint8_t p = 0; p < AHCI_MAX_PORTS; p++) {
            if (!ctrl->ports[p].present) continue;
            if (letter > 'Z') return VFS_ERR_NO_SLOTS;

            vfs_dev_addr_t addr = { .ahci = { .controller = c, .port = p } };
            uint8_t err = vfs_mount(AHCI, letter, addr);
            if (err == VFS_ERR_NO_SLOTS) return VFS_ERR_NO_SLOTS;
            letter++;
        }
    }

    uint8_t nvme_count = nvme_get_controller_count();
    for (uint8_t c = 0; c < nvme_count; c++) {
        nvme_controller_t *ctrl = nvme_get_controller(c);
        if (!ctrl) continue;

        for (uint32_t ns = 1; ns <= nvme_get_namespace_count(ctrl); ns++) {
            if (!nvme_namespace_present(ctrl, ns)) continue;
            if (letter > 'Z') return VFS_ERR_NO_SLOTS;

            vfs_dev_addr_t addr = { .nvme = { .controller = c, .nsid = ns } };
            uint8_t err = vfs_mount(NVME, letter, addr);
            if (err == VFS_ERR_NO_SLOTS) return VFS_ERR_NO_SLOTS;
            letter++;
        }
    }

    for (uint8_t bus = 0; bus <= 1; bus++) {
        for (uint8_t drive = 0; drive <= 1; drive++) {
            if (!ide_drive_present(bus, drive)) continue;
            if (letter > 'Z') return VFS_ERR_NO_SLOTS;

            vfs_dev_addr_t addr = { .ide = { .bus = bus, .drive = drive } };
            uint8_t err = vfs_mount(IDE, letter, addr);
            if (err == VFS_ERR_NO_SLOTS) return VFS_ERR_NO_SLOTS;
            letter++;
        }
    }

    return VFS_OK;
}