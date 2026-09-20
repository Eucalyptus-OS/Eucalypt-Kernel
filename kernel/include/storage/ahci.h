#pragma once

#include <stdint.h>

// HBA limits: up to 8 controllers, 32 ports each, 32 command slots per port
#define AHCI_MAX_CONTROLLERS    8
#define AHCI_MAX_PORTS          32
#define AHCI_MAX_CMD_SLOTS      32
#define AHCI_SECTOR_SIZE        512
// One PRD covers at most 0x2000 bytes (16 sectors); max 8 PRDs per command table
#define AHCI_CMD_TBL_PRDT_ENTRIES 8
#define AHCI_MAX_PRDT_ENTRIES    8
#define AHCI_PRDT_MAX_BYTES      0x2000

// FIS (Frame Information Structure) type identifiers defined by the SATA spec
typedef enum {
    FIS_TYPE_REG_H2D    = 0x27,  // register - host to device
    FIS_TYPE_REG_D2H    = 0x34,  // register - device to host
    FIS_TYPE_DMA_ACT    = 0x39,  // DMA activate - device to host
    FIS_TYPE_DMA_SETUP  = 0x41,  // DMA setup - device to host
    FIS_TYPE_DATA       = 0x46,  // data - bidirectional
    FIS_TYPE_BIST       = 0x58,  // BIST activate
    FIS_TYPE_PIO_SETUP  = 0x5F,  // PIO setup - device to host
    FIS_TYPE_DEV_BITS   = 0xA1   // set device bits - device to host
} FIS_TYPE;

// ATA command opcodes used by the driver
#define ATA_CMD_IDENTIFY       0xEC  // return the 512-byte identify data block
#define ATA_CMD_READ_DMA_EXT   0x25  // 48-bit LBA read via DMA
#define ATA_CMD_WRITE_DMA_EXT  0x35  // 48-bit LBA write via DMA

// PxSSTS fields: DET = device detection state, IPM = interface power management state
#define HBA_PORT_DET_PRESENT   3
#define HBA_PORT_IPM_ACTIVE    1

// PxCMD command-register bits
#define HBA_PxCMD_ST   (1 << 0)   // start command engine
#define HBA_PxCMD_SUD  (1 << 1)   // spin-up device
#define HBA_PxCMD_POD  (1 << 2)   // power-on device
#define HBA_PxCMD_FRE  (1 << 4)   // FIS receive enable
#define HBA_PxCMD_FR   (1 << 14)  // FIS receive running
#define HBA_PxCMD_CR   (1 << 15)  // command list running

// PxIS interrupt status bits; IFS/HBDS/HBFS/TFES are the command-failure bits
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
#define HBA_PxIS_IFS   (1 << 27)  // interface fatal error
#define HBA_PxIS_HBDS  (1 << 28)  // host bus data error
#define HBA_PxIS_HBFS  (1 << 29)  // host bus fatal error
#define HBA_PxIS_TFES  (1 << 30)  // task file error (device reported an error)

// Port signature (PxSIG) values identifying the type of connected device
#define HBA_PORT_SIG_ATA   0x00000101
#define HBA_PORT_SIG_ATAPI 0xEB140101
#define HBA_PORT_SIG_SEMB  0xC33C0101
#define HBA_PORT_SIG_PM    0x96690101

// check_type() results for the device attached to a port
#define AHCI_DEV_NULL   0
#define AHCI_DEV_SATA   1
#define AHCI_DEV_SEMB   2
#define AHCI_DEV_PM     3
#define AHCI_DEV_SATAPI 4

// PxTFD status bits read while waiting for the drive to accept a command
#define ATA_DEV_BUSY 0x80  // device busy (BSY)
#define ATA_DEV_DRQ  0x08  // data request in progress (DRQ)

// Physical base address where the CLB/FIS/command-table memory region is carved out
#define AHCI_BASE 0x400000

// Geometry and location of one discovered drive
typedef struct drive {
    uint64_t sector_count;  // total sectors (48-bit LBA)
    uint32_t sector_size;   // bytes per sector (usually 512)
    uint8_t controller;     // AHCI controller index
    uint8_t port;           // port index on that controller
    uint8_t device_type;    // AHCI_DEV_* type
} drive_t;

// Command-list header: one per slot, links the slot to its command table
typedef struct {
    uint16_t cfl : 5;   // command FIS length in dwords (5 for a 20-byte H2D FIS)
    uint16_t a   : 1;   // ATAPI
    uint16_t w   : 1;   // 1 = write transfer, 0 = read
    uint16_t p   : 1;   // prefetch
    uint16_t r   : 1;   // reset
    uint16_t b   : 1;   // busy
    uint16_t c   : 1;   // clear busy on error
    uint16_t res0: 1;
    uint16_t pmp : 4;   // port-multiplier port (attached device)
    uint16_t prdtl;     // number of PRDT entries in the command table
    volatile uint32_t prdbc;  // bytes transferred by the controller so far
    uint32_t ctba;      // command table base address, bits 0-31
    uint32_t ctbau;     // command table base address, bits 32-63
    uint32_t res1[4];
} HBA_CMD_HEADER;

// Physical region descriptor: points at one contiguous DMA buffer
typedef struct {
    uint32_t dba;        // data buffer base address, bits 0-31
    uint32_t dbau;       // data buffer base address, bits 32-63
    uint32_t reserved;
    uint32_t dbc : 22;   // byte count - 1 (max 4MB per entry)
    uint32_t reserved1 : 9;
    uint32_t i : 1;      // interrupt on completion; set on the last entry
} HBA_PRDT_ENTRY;

// Command table: command FIS + ATAPI command + PRD array describing the data buffers
typedef struct {
    uint8_t cfis[64];                     // the FIS_REG_H2D command FIS lives here
    uint8_t acmd[16];                     // ATAPI command bytes (unused for plain ATA)
    uint8_t reserved[48];
    HBA_PRDT_ENTRY prdt_entry[AHCI_CMD_TBL_PRDT_ENTRIES];
} HBA_CMD_TBL;

// Register FIS sent host -> device to issue an ATA command
typedef struct __attribute__((packed)) {
    uint8_t fis_type;         // FIS_TYPE_REG_H2D
    uint8_t pmport : 4;       // destination port-multiplier port
    uint8_t rsv0 : 3;
    uint8_t c : 1;            // 1 = command FIS, 0 = device-control register write
    uint8_t command;          // ATA command opcode
    uint8_t featurel;
    uint8_t lba0;             // LBA bits 0-7
    uint8_t lba1;             // LBA bits 8-15
    uint8_t lba2;             // LBA bits 16-23
    uint8_t device;           // device register; bit 6 selects LBA addressing mode
    uint8_t lba3;             // LBA bits 24-31
    uint8_t lba4;             // LBA bits 32-39 (0 for 28-bit LBA)
    uint8_t lba5;             // LBA bits 40-47
    uint8_t featureh;
    uint8_t countl;           // sector count, low byte
    uint8_t counth;           // sector count, high byte
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} FIS_REG_H2D;

// Register FIS sent device -> host carrying the final task-file status
typedef struct __attribute__((packed)) {
    uint8_t fis_type;         // FIS_TYPE_REG_D2H
    uint8_t pmport : 4;
    uint8_t i : 1;            // interrupt bit
    uint8_t rsv0 : 1;
    uint8_t rsv1 : 2;
    uint8_t status;           // drive status register after the command
    uint8_t error;            // error register when status.ERR is set
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

// Set-device-bits FIS: device -> host, delivers status bits without a full D2H
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

// FIS receive area: device-to-host FISes land here, with RFIS holding the latest status
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

// One port's register block within the HBA MMIO region (layout defined by the AHCI spec)
typedef struct {
    volatile uint32_t clb;   // command list base address, low 32 bits
    volatile uint32_t clbu;  // command list base address, high 32 bits
    volatile uint32_t fb;    // FIS base address, low 32 bits
    volatile uint32_t fbu;   // FIS base address, high 32 bits
    volatile uint32_t is;    // interrupt status (write 1 to clear)
    volatile uint32_t ie;    // interrupt enable
    volatile uint32_t cmd;   // command register (ST/FRE/FR/CR bits)
    volatile uint32_t rsv0;
    volatile uint32_t tfd;   // task file data: status in low byte, error in next byte
    volatile uint32_t sig;   // signature identifying the attached device
    volatile uint32_t ssts;  // serial ATA status (DET/IPM fields)
    volatile uint32_t sctl;  // serial ATA control
    volatile uint32_t serr;  // serial ATA error register
    volatile uint32_t sact;  // SATA active: set bit i while command slot i is active
    volatile uint32_t ci;    // command issue: set bit i to run command slot i
    volatile uint32_t sntf;  // SATA notification
    volatile uint32_t fbs;   // FIS-based switching
    volatile uint32_t rsv1[11];
    volatile uint32_t vendor[4];
} HBA_PORT;

// HBA memory-mapped registers (typically PCI BAR5)
typedef struct {
    volatile uint32_t cap;    // host capabilities
    volatile uint32_t ghc;    // global HBA control (AE = enable, IE = interrupts)
    volatile uint32_t is;     // global interrupt status, one bit per port
    volatile uint32_t pi;     // ports implemented bitmap
    volatile uint32_t vs;     // version
    volatile uint32_t ccc_ctl;
    volatile uint32_t ccc_pts;
    volatile uint32_t em_loc;
    volatile uint32_t em_ctl;
    volatile uint32_t cap2;   // capability flags (bit 0 = BIOS/OS handoff supported)
    volatile uint32_t bohc;   // BIOS/OS handoff control
    uint8_t reserved[0xD4];
    HBA_PORT ports[AHCI_MAX_PORTS];
} HBA_MEM;

// Per-port housekeeping for a controller
typedef struct {
    uint8_t type;         // AHCI_DEV_* device type
    uint8_t present;      // 1 if a device is attached to this port
    char    *assigned_name;
} ahci_port_info_t;

// One AHCI controller instance discovered on the PCI bus
typedef struct {
    HBA_MEM *abar;
    ahci_port_info_t ports[AHCI_MAX_PORTS];
} ahci_controller_t;

// Global AHCI driver state
typedef struct {
    ahci_controller_t controllers[AHCI_MAX_CONTROLLERS];
    uint8_t count;
} ahci_state_t;

// Completion callback fired from the ISR with the slot index and error status
typedef void (*ahci_callback_t)(uint8_t controller, uint8_t port, uint8_t slot, uint8_t status, void *ctx);
// Queue an asynchronous command; the transfer finishes via `callback`
int ahci_read(uint8_t controller, uint8_t port, uint64_t sector, uint8_t count, void *buf,
              ahci_callback_t callback, void *ctx);
int ahci_write(uint8_t controller, uint8_t port, uint64_t sector, uint8_t count, const void *buf,
               ahci_callback_t callback, void *ctx);
uint8_t ahci_init();
uint8_t ahci_get_controller_count();
ahci_controller_t *ahci_get_controller(uint8_t index);
uint8_t ahci_get_port_count(uint8_t controller);
uint8_t ahci_get_port_index(uint8_t controller, uint8_t n);
drive_t *ahci_get_drive(uint8_t controller, uint8_t port);