#include <stdint.h>
#include <mm/heap.h>
#include <drivers/block/ahci.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/fs/vfs/blockdev.h>
#include <drivers/fs/fat16/fat16.h>

#define MAX_DRIVES 255

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
    ahci_blockdev_priv_t *priv = (ahci_blockdev_priv_t *)dev->priv;
    return ahci_read(priv->controller, priv->port, lba, count, buf);
}

static uint8_t ahci_blockdev_write(vfs_blockdev_t *dev, uint32_t lba, uint8_t count, const void *buf) {
    ahci_blockdev_priv_t *priv = (ahci_blockdev_priv_t *)dev->priv;
    return ahci_write(priv->controller, priv->port, lba, count, buf);
}

uint8_t vfs_mount(enum StorageDevType dev, char letter, uint8_t controller, uint8_t port) {
    if (find_letter_slot(letter) >= 0)
        return VFS_ERR_LETTER_IN_USE;

    if (mount_count >= MAX_DRIVES)
        return VFS_ERR_NO_SLOTS;

    vfs_blockdev_t blockdev = {0};

    if (dev == AHCI) {
        if (controller >= ahci_get_controller_count())
            return VFS_ERR_INVALID_DEV;

        ahci_controller_t *c = ahci_get_controller(controller);
        if (!c)
            return VFS_ERR_INVALID_DEV;

        if (!c->ports[port].present)
            return VFS_ERR_INVALID_DEV;

        if (c->ports[port].assigned_letter != 0)
            return VFS_ERR_ALREADY_MOUNTED;

        ahci_blockdev_priv_t *priv = kmalloc(sizeof(ahci_blockdev_priv_t));
        if (!priv)
            return VFS_ERR_NO_SLOTS;

        priv->controller = controller;
        priv->port       = port;
        blockdev.read    = ahci_blockdev_read;
        blockdev.write   = ahci_blockdev_write;
        blockdev.priv    = priv;

        if (fat16_init(&blockdev) != 0) {
            kfree(priv);
            return VFS_ERR_FS_INIT;
        }

        c->ports[port].assigned_letter = letter;

    } else {
        return VFS_ERR_UNKNOWN_DEV;
    }

    mount_table[mount_count].letter  = letter;
    mount_table[mount_count].blockdev = blockdev;
    mount_table[mount_count].dev     = dev;
    mount_count++;

    return VFS_OK;
}

void vfs_unmount(char letter) {
    int slot = find_letter_slot(letter);
    if (slot < 0) return;

    vfs_mount_t *m = &mount_table[slot];

    if (m->dev == AHCI) {
        ahci_blockdev_priv_t *priv = (ahci_blockdev_priv_t *)m->blockdev.priv;
        ahci_controller_t *c = ahci_get_controller(priv->controller);
        if (c) c->ports[priv->port].assigned_letter = 0;
        kfree(priv);
    }

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
            vfs_mount(AHCI, letter, c, p);
            letter++;
        }
    }

    return VFS_OK;
}