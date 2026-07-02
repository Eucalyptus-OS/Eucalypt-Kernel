#include <stdint.h>
#include <portio.h>
#include <mem.h>
#include <drivers/block/ide.h>

#define ATA_CMD_READ_PIO          0x20
#define ATA_CMD_READ_PIO_EXT      0x24
#define ATA_CMD_READ_DMA          0xC8
#define ATA_CMD_READ_DMA_EXT      0x25
#define ATA_CMD_WRITE_PIO         0x30
#define ATA_CMD_WRITE_PIO_EXT     0x34
#define ATA_CMD_WRITE_DMA         0xCA
#define ATA_CMD_WRITE_DMA_EXT     0x35
#define ATA_CMD_CACHE_FLUSH       0xE7
#define ATA_CMD_CACHE_FLUSH_EXT   0xEA
#define ATA_CMD_PACKET            0xA0
#define ATA_CMD_IDENTIFY_PACKET   0xA1
#define ATA_CMD_IDENTIFY          0xEC
#define ATA_IDENT_DEVICETYPE   0
#define ATA_IDENT_CYLINDERS    2
#define ATA_IDENT_HEADS        6
#define ATA_IDENT_SECTORS      12
#define ATA_IDENT_SERIAL       20
#define ATA_IDENT_MODEL        54
#define ATA_IDENT_CAPABILITIES 98
#define ATA_IDENT_FIELDVALID   106
#define ATA_IDENT_MAX_LBA      120
#define ATA_IDENT_COMMANDSETS  164
#define ATA_IDENT_MAX_LBA_EXT  200
#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_FEATURES   0x01
#define ATA_REG_SECCOUNT0  0x02
#define ATA_REG_LBA0       0x03
#define ATA_REG_LBA1       0x04
#define ATA_REG_LBA2       0x05
#define ATA_REG_HDDEVSEL   0x06
#define ATA_REG_COMMAND    0x07
#define ATA_REG_STATUS     0x07
#define ATA_REG_SECCOUNT1  0x08
#define ATA_REG_LBA3       0x09
#define ATA_REG_LBA4       0x0A
#define ATA_REG_LBA5       0x0B
#define ATA_REG_CONTROL    0x0C
#define ATA_REG_ALTSTATUS  0x0C
#define ATA_REG_DEVADDRESS 0x0D
#define ATA_PRIMARY      0x00
#define ATA_SECONDARY    0x01
#define ATA_READ      0x00
#define ATA_WRITE     0x01
#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DF      0x20
#define ATA_SR_DSC     0x10
#define ATA_SR_DRQ     0x08
#define ATA_SR_CORR    0x04
#define ATA_SR_IDX     0x02
#define ATA_SR_ERR     0x01
#define ATA_ER_BBK     0x80
#define ATA_ER_UNC     0x40
#define ATA_ER_MC      0x20
#define ATA_ER_IDNF    0x10
#define ATA_ER_MCR     0x08
#define ATA_ER_ABRT    0x04
#define ATA_ER_TK0NF   0x02
#define ATA_ER_AMNF    0x01
#define IDE_ATA     0x00
#define IDE_ATAPI   0x01

#define IDE_POLL_TIMEOUT_ITERS 2000000
#define IDE_STATUS_FLOATING    0xFF

uint8_t ide_buf[2048] = {0};
uint8_t ide_count = 0;
volatile unsigned static char ide_irq = 0;

static ide_state_t ide_state = {0};

struct ide_channel_regs {
    uint16_t base;
    uint16_t ctrl;
    uint16_t bmide;
    uint8_t  n_ien;
} channels [2];

struct ide_device {
    uint8_t reserved;
    uint8_t channel;
    uint8_t drive;
    uint16_t type;
    uint16_t sig;
    uint16_t capabilities;
    uint32_t commands;
    uint32_t size;
    uint8_t model[41];
} ide_devicesp[4];

void ide_write_reg(uint8_t channel, uint8_t reg, uint8_t data) {
    if (reg > 0x07 && reg < 0x0C)
        ide_write_reg(channel, ATA_REG_CONTROL, 0x80 | channels[channel].n_ien);
    if (reg < 0x08)
        outb(channels[channel].base  + reg - 0x00, data);
    else if (reg < 0x0C)
        outb(channels[channel].base  + reg - 0x06, data);
    else if (reg < 0x0E)
        outb(channels[channel].ctrl  + reg - 0x0A, data);
    else if (reg < 0x16)
        outb(channels[channel].bmide + reg - 0x0E, data);
    if (reg > 0x07 && reg < 0x0C)
        ide_write_reg(channel, ATA_REG_CONTROL, channels[channel].n_ien);
}

uint8_t ide_read_reg(uint8_t channel, uint8_t reg) {
    uint8_t res = '\0';
    if (reg > 0x07 && reg < 0x0C)
        ide_write_reg(channel, ATA_REG_CONTROL, 0x80 | channels[channel].n_ien);
    if (reg < 0x08)
        res = inb(channels[channel].base + reg - 0x00);
    else if (reg < 0x0C)
        res = inb(channels[channel].base  + reg - 0x06);
    else if (reg < 0x0E)
        res = inb(channels[channel].ctrl  + reg - 0x0A);
    else if (reg < 0x16)
        res = inb(channels[channel].bmide + reg - 0x0E);
    if (reg > 0x07 && reg < 0x0C)
        ide_write_reg(channel, ATA_REG_CONTROL, channels[channel].n_ien);
    return res;
}

void ide_read_buffer(unsigned char channel, unsigned char reg,
                      void *buffer, unsigned int quads) {
    if (reg > 0x07 && reg < 0x0C)
        ide_write_reg(channel, ATA_REG_CONTROL, 0x80 | channels[channel].n_ien);

    uint16_t port = 0;
    if (reg < 0x08) {
        port = channels[channel].base  + reg - 0x00;
    } else if (reg < 0x0C) {
        port = channels[channel].base  + reg - 0x06;
    } else if (reg < 0x0E) {
        port = channels[channel].ctrl  + reg - 0x0A;
    } else if (reg < 0x16) {
        port = channels[channel].bmide + reg - 0x0E;
    }

    asm volatile (
        "rep insl"
        : "+D"(buffer), "+c"(quads)
        : "d"(port)
        : "memory"
    );

    if (reg > 0x07 && reg < 0x0C)
        ide_write_reg(channel, ATA_REG_CONTROL, channels[channel].n_ien);
}

static uint8_t ide_channel_floating(uint8_t channel) {
    return ide_read_reg(channel, ATA_REG_STATUS) == IDE_STATUS_FLOATING;
}

uint8_t ide_polling(uint8_t channel, uint32_t advanced_check) {
    if (ide_channel_floating(channel)) {
        return 4;
    }

    for (int i = 0; i < 4; i++)
        ide_read_reg(channel, ATA_REG_ALTSTATUS);

    uint32_t spin = 0;
    while (ide_read_reg(channel, ATA_REG_STATUS) & ATA_SR_BSY) {
        if (++spin > IDE_POLL_TIMEOUT_ITERS) {
            return 4;
        }
    }

    if (advanced_check) {
        uint8_t state = ide_read_reg(channel, ATA_REG_STATUS);
        if (state & ATA_SR_ERR) {
            return 2;
        }
        if (state & ATA_SR_DF) {
            return 1;
        }
        if ((state & ATA_SR_DRQ) == 0) {
            return 3;
        }
    }
    return 0;
}

uint8_t ide_init(uint32_t bar0, uint32_t bar1, uint32_t bar2, uint32_t bar3,
                  uint32_t bar4) {
    int i, j, c = 0;

    channels[ATA_PRIMARY  ].base  = (bar0 & 0xFFFFFFFC) + 0x1F0 * (!bar0);
    channels[ATA_PRIMARY  ].ctrl  = (bar1 & 0xFFFFFFFC) + 0x3F6 * (!bar1);
    channels[ATA_SECONDARY].base  = (bar2 & 0xFFFFFFFC) + 0x170 * (!bar2);
    channels[ATA_SECONDARY].ctrl  = (bar3 & 0xFFFFFFFC) + 0x376 * (!bar3);
    channels[ATA_PRIMARY  ].bmide = (bar4 & 0xFFFFFFFC) + 0;
    channels[ATA_SECONDARY].bmide = (bar4 & 0xFFFFFFFC) + 8;

    ide_write_reg(ATA_PRIMARY  , ATA_REG_CONTROL, 2);
    ide_write_reg(ATA_SECONDARY, ATA_REG_CONTROL, 2);

    for (i = 0; i < 2; i++) {
        if (ide_channel_floating(i)) {
            continue;
        }

        for (j = 0; j < 2; j++) {
            uint8_t type = IDE_ATA;

            ide_write_reg(i, ATA_REG_HDDEVSEL, 0xA0 | (j << 4));
            for (int k = 0; k < 4; k++)
                ide_read_reg(i, ATA_REG_ALTSTATUS);

            if (ide_channel_floating(i)) {
                continue;
            }

            ide_write_reg(i, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

            if (ide_polling(i, 1)) {
                continue;
            }

            ide_count++;

            ide_read_buffer(i, ATA_REG_DATA, (void*)ide_buf, 128);

            ide_devicesp[c].type = type;
            ide_devicesp[c].channel = i;
            ide_devicesp[c].drive = j;
            ide_devicesp[c].sig = *((uint16_t *)(ide_buf + ATA_IDENT_DEVICETYPE));
            ide_devicesp[c].capabilities = *((uint16_t *)(ide_buf + ATA_IDENT_CAPABILITIES));
            ide_devicesp[c].commands = *((uint32_t *)(ide_buf + ATA_IDENT_COMMANDSETS));
            ide_devicesp[c].size = *((uint32_t *)(ide_buf + ATA_IDENT_MAX_LBA));

            if (ide_devicesp[c].commands & 0x0400000)
                ide_devicesp[c].size = *((uint32_t *)(ide_buf + ATA_IDENT_MAX_LBA_EXT));

            for (int k = 0; k < 40; k += 2) {
                ide_devicesp[c].model[k] = ide_buf[ATA_IDENT_MODEL + k + 1];
                ide_devicesp[c].model[k + 1] = ide_buf[ATA_IDENT_MODEL + k];
            }
            ide_devicesp[c].model[40] = 0;

            ide_state.drives[c].type = type;
            ide_state.drives[c].present = 1;
            ide_state.drives[c].number = c;
            ide_state.drives[c].assigned_letter = 0;

            c++;
        }
    }

    for (; c < 4; c++) {
        ide_devicesp[c].type = 0;
        ide_state.drives[c].present = 0;
        ide_state.drives[c].assigned_letter = 0;
    }

    return 0;
}

uint8_t ide_read(uint8_t port, uint8_t drive, uint64_t sector, uint8_t count, void *buf) {
    sector = (uint32_t)sector;
    if (port > 1 || drive > 1 || count == 0) {
        return 1;
    }
    if (!ide_drive_present(port, drive)) {
        return 1;
    }

    uint8_t channel = port;
    uint8_t slave = drive;

    if (ide_channel_floating(channel)) {
        return 1;
    }

    ide_write_reg(channel, ATA_REG_CONTROL, channels[channel].n_ien = (ide_irq == 0) ? 1 : 0);
    if (ide_polling(channel, 0)) {
        return 1;
    }

    ide_write_reg(channel, ATA_REG_FEATURES, 0);
    ide_write_reg(channel, ATA_REG_SECCOUNT0, count);
    ide_write_reg(channel, ATA_REG_LBA0, (uint8_t)sector);
    ide_write_reg(channel, ATA_REG_LBA1, (uint8_t)(sector >> 8));
    ide_write_reg(channel, ATA_REG_LBA2, (uint8_t)(sector >> 16));
    ide_write_reg(channel, ATA_REG_HDDEVSEL, 0xE0 | (slave << 4) | (uint8_t)(sector >> 24));
    ide_write_reg(channel, ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    if (ide_polling(channel, 1)) {
        return 1;
    }

    int size = count * 512 / 2;
    ide_read_buffer(channel, ATA_REG_DATA, buf, size);

    return 0;
}

uint8_t ide_write(uint8_t port, uint8_t drive, uint64_t sector, uint8_t count, const void *buf) {
    sector = (uint32_t)sector;
    if (port > 1 || drive > 1 || count == 0) {
        return 1;
    }
    if (!ide_drive_present(port, drive)) {
        return 1;
    }

    uint8_t channel = port;
    uint8_t slave = drive;

    if (ide_channel_floating(channel)) {
        return 1;
    }

    ide_write_reg(channel, ATA_REG_CONTROL, channels[channel].n_ien = (ide_irq == 0) ? 1 : 0);
    if (ide_polling(channel, 0)) {
        return 1;
    }

    ide_write_reg(channel, ATA_REG_FEATURES, 0);
    ide_write_reg(channel, ATA_REG_SECCOUNT0, count);
    ide_write_reg(channel, ATA_REG_LBA0, (uint8_t)sector);
    ide_write_reg(channel, ATA_REG_LBA1, (uint8_t)(sector >> 8));
    ide_write_reg(channel, ATA_REG_LBA2, (uint8_t)(sector >> 16));
    ide_write_reg(channel, ATA_REG_HDDEVSEL, 0xE0 | (slave << 4) | (uint8_t)(sector >> 24));
    ide_write_reg(channel, ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    if (ide_polling(channel, 0)) {
        return 1;
    }

    int size = count * 512 / 2;
    asm volatile (
        "rep outsl"
        : "+S"(buf), "+c"(size)
        : "d"(channels[channel].base + ATA_REG_DATA)
        : "memory"
    );

    ide_polling(channel, 1);

    return 0;
}

uint8_t ide_drive_present(uint8_t port, uint8_t drive) {
    if (port > 1 || drive > 1) {
        return 0;
    }

    uint8_t index = port * 2 + drive;
    if (index >= 4) {
        return 0;
    }

    return ide_state.drives[index].present;
}

char ide_drive_letter(uint8_t port, uint8_t drive) {
    if (port > 1 || drive > 1) {
        return 0;
    }

    uint8_t index = port * 2 + drive;
    if (index >= 4) {
        return 0;
    }

    return ide_state.drives[index].assigned_letter;
}

void ide_set_drive_letter(uint8_t port, uint8_t drive, char letter) {
    if (port > 1 || drive > 1) {
        return;
    }

    uint8_t index = port * 2 + drive;
    if (index >= 4) {
        return;
    }

    ide_state.drives[index].assigned_letter = letter;
}

ide_drive_info_t *ide_get_device(uint8_t index) {
    if (index >= 4) {
        return NULL;
    }
    return &ide_state.drives[index];
}