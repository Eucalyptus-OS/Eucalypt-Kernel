#pragma once

#include <stdint.h>

#define AHCI_MAX_CONTROLLERS    8
#define AHCI_MAX_PORTS          32
#define AHCI_MAX_CMD_SLOTS      32
#define AHCI_SECTOR_SIZE        512
#define AHCI_CMD_TBL_PRDT_ENTRIES 8
#define AHCI_MAX_PRDT_ENTRIES    8
#define AHCI_PRDT_MAX_BYTES      0x2000

typedef enum {
    FIS_TYPE_REG_H2D    = 0x27,
    FIS_TYPE_REG_D2H    = 0x34,
    FIS_TYPE_DMA_ACT    = 0x39,
    FIS_TYPE_DMA_SETUP  = 0x41,
    FIS_TYPE_DATA       = 0x46,
    FIS_TYPE_BIST       = 0x58,
    FIS_TYPE_PIO_SETUP  = 0x5F,
    FIS_TYPE_DEV_BITS   = 0xA1
} FIS_TYPE;

#define ATA_CMD_IDENTIFY       0xEC
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35

#define HBA_PORT_DET_PRESENT   3
#define HBA_PORT_IPM_ACTIVE    1

#define HBA_PxCMD_ST   (1 << 0)
#define HBA_PxCMD_SUD  (1 << 1)
#define HBA_PxCMD_POD  (1 << 2)
#define HBA_PxCMD_FRE  (1 << 4)
#define HBA_PxCMD_FR   (1 << 14)
#define HBA_PxCMD_CR   (1 << 15)

#define HBA_PxIS_DHRS  (1 << 0)
#define HBA_PxIS_PSS   (1 << 1)
#define HBA_PxIS_SDBS  (1 << 2)
#define HBA_PxIS_UFS   (1 << 3)
#define HBA_PxIS_DSS   (1 << 4)
#define HBA_PxIS_PMS   (1 << 5)
#define HBA_PxIS_PCS   (1 << 6)
#define HBA_PxIS_DPS   (1 << 7)
#define HBA_PxIS_UES   (1 << 8)
#define HBA_PxIS_PRS   (1 << 9)
#define HBA_PxIS_DMAS  (1 << 10)
#define HBA_PxIS_SSS   (1 << 11)
#define HBA_PxIS_PSSS  (1 << 12)
#define HBA_PxIS_SDBS2 (1 << 13)
#define HBA_PxIS_IFS   (1 << 27)
#define HBA_PxIS_HBDS  (1 << 28)
#define HBA_PxIS_HBFS  (1 << 29)
#define HBA_PxIS_TFES  (1 << 30)

#define HBA_PORT_SIG_ATA   0x00000101
#define HBA_PORT_SIG_ATAPI 0xEB140101
#define HBA_PORT_SIG_SEMB  0xC33C0101
#define HBA_PORT_SIG_PM    0x96690101

#define AHCI_DEV_NULL   0
#define AHCI_DEV_SATA   1
#define AHCI_DEV_SEMB   2
#define AHCI_DEV_PM     3
#define AHCI_DEV_SATAPI 4

#define ATA_DEV_BUSY 0x80
#define ATA_DEV_DRQ  0x08

#define AHCI_BASE 0x400000

typedef struct {
    uint16_t cfl : 5;
    uint16_t a   : 1;
    uint16_t w   : 1;
    uint16_t p   : 1;
    uint16_t r   : 1;
    uint16_t b   : 1;
    uint16_t c   : 1;
    uint16_t res0: 1;
    uint16_t pmp : 4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t res1[4];
} HBA_CMD_HEADER;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc : 22;
    uint32_t reserved1 : 9;
    uint32_t i : 1;
} HBA_PRDT_ENTRY;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    HBA_PRDT_ENTRY prdt_entry[AHCI_CMD_TBL_PRDT_ENTRIES];
} HBA_CMD_TBL;

typedef struct __attribute__((packed)) {
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t rsv0 : 3;
    uint8_t c : 1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} FIS_REG_H2D;

typedef struct __attribute__((packed)) {
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t i : 1;
    uint8_t rsv0 : 1;
    uint8_t rsv1 : 2;
    uint8_t status;
    uint8_t error;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t reserved;
    uint8_t countl;
    uint8_t counth;
    uint8_t reserved2[2];
} FIS_REG_D2H;

typedef struct __attribute__((packed)) {
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t rsv0 : 3;
    uint8_t i : 1;
    uint8_t status;
    uint8_t error;
    uint32_t lba;
    uint16_t count;
    uint16_t reserved;
    uint8_t reserved2[4];
} FIS_DEV_BITS;

typedef struct {
    uint8_t dsfis[0x1c];
    uint8_t pad0[0x04];
    uint8_t psfis[0x14];
    uint8_t pad1[0x04];
    uint8_t rfis[0x14];
    uint8_t pad2[0x04];
    uint8_t sdbfis[0x08];
    uint8_t pad3[0x04];
    uint8_t ufis[0x40];
    uint8_t rsv[0x60];
} HBA_FIS;

typedef struct {
    volatile uint32_t clb;
    volatile uint32_t clbu;
    volatile uint32_t fb;
    volatile uint32_t fbu;
    volatile uint32_t is;
    volatile uint32_t ie;
    volatile uint32_t cmd;
    volatile uint32_t rsv0;
    volatile uint32_t tfd;
    volatile uint32_t sig;
    volatile uint32_t ssts;
    volatile uint32_t sctl;
    volatile uint32_t serr;
    volatile uint32_t sact;
    volatile uint32_t ci;
    volatile uint32_t sntf;
    volatile uint32_t fbs;
    volatile uint32_t rsv1[11];
    volatile uint32_t vendor[4];
} HBA_PORT;

typedef struct {
    volatile uint32_t cap;
    volatile uint32_t ghc;
    volatile uint32_t is;
    volatile uint32_t pi;
    volatile uint32_t vs;
    volatile uint32_t ccc_ctl;
    volatile uint32_t ccc_pts;
    volatile uint32_t em_loc;
    volatile uint32_t em_ctl;
    volatile uint32_t cap2;
    volatile uint32_t bohc;
    uint8_t reserved[0xD4];
    HBA_PORT ports[AHCI_MAX_PORTS];
} HBA_MEM;

typedef struct {
    uint8_t type;
    uint8_t present;
    char    assigned_letter;
} ahci_port_info_t;

typedef struct {
    HBA_MEM *abar;
    ahci_port_info_t ports[AHCI_MAX_PORTS];
} ahci_controller_t;

typedef struct {
    ahci_controller_t controllers[AHCI_MAX_CONTROLLERS];
    uint8_t count;
} ahci_state_t;

uint8_t ahci_read(uint8_t controller, uint8_t port, uint32_t sector, uint8_t count, void *buf);
uint8_t ahci_write(uint8_t controller, uint8_t port, uint32_t sector, uint8_t count, const void *buf);
uint8_t ahci_init();
uint8_t ahci_get_controller_count();
ahci_controller_t *ahci_get_controller(uint8_t index);