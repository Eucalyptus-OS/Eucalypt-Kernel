#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// PCI configuration space I/O ports
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

// Sentinel returned when no device is present at a slot
#define PCI_VENDOR_NONE 0xFFFF

// Bit in the header type field that indicates a multifunction device
#define PCI_HEADER_MULTIFUNCTION 0x80

// PCI header type values (bits 6:0, masking out the multifunction bit)
#define PCI_HEADER_TYPE_STANDARD 0x00
#define PCI_HEADER_TYPE_BRIDGE   0x01
#define PCI_HEADER_TYPE_CARDBUS  0x02

// Standard type 0 (endpoint) configuration space register offsets
#define PCI_OFFSET_VENDOR_ID    0x00
#define PCI_OFFSET_DEVICE_ID    0x02
#define PCI_OFFSET_COMMAND      0x04
#define PCI_OFFSET_STATUS       0x06
#define PCI_OFFSET_REVISION_ID  0x08
#define PCI_OFFSET_PROG_IF      0x09
#define PCI_OFFSET_SUBCLASS     0x0A
#define PCI_OFFSET_CLASS_CODE   0x0B
#define PCI_OFFSET_CACHE_LINE   0x0C
#define PCI_OFFSET_LATENCY      0x0D
#define PCI_OFFSET_HEADER_TYPE  0x0E
#define PCI_OFFSET_BIST         0x0F
#define PCI_OFFSET_BAR0         0x10
#define PCI_OFFSET_BAR1         0x14
#define PCI_OFFSET_BAR2         0x18
#define PCI_OFFSET_BAR3         0x1C
#define PCI_OFFSET_BAR4         0x20
#define PCI_OFFSET_BAR5         0x24
#define PCI_OFFSET_SUBSYS_VID   0x2C
#define PCI_OFFSET_SUBSYS_ID    0x2E
#define PCI_OFFSET_ROM_BASE     0x30
#define PCI_OFFSET_CAP_PTR      0x34
#define PCI_OFFSET_IRQ_LINE     0x3C
#define PCI_OFFSET_IRQ_PIN      0x3D

// Type 1 (PCI-to-PCI bridge) additional register offsets
#define PCI_OFFSET_PRIMARY_BUS   0x18
#define PCI_OFFSET_SECONDARY_BUS 0x19
#define PCI_OFFSET_SUBORD_BUS    0x1A

// PCI command register bits
#define PCI_COMMAND_IO_SPACE       (1 << 0)
#define PCI_COMMAND_MEM_SPACE      (1 << 1)
#define PCI_COMMAND_BUS_MASTER     (1 << 2)
#define PCI_COMMAND_INT_DISABLE    (1 << 10)

// PCI class codes
#define PCI_CLASS_STORAGE      0x01
#define PCI_CLASS_NETWORK      0x02
#define PCI_CLASS_DISPLAY      0x03
#define PCI_CLASS_BRIDGE       0x06
#define PCI_CLASS_SERIAL       0x0C

// PCI subclass codes — storage (class 0x01)
#define PCI_SUBCLASS_IDE       0x01
#define PCI_SUBCLASS_AHCI      0x06

// PCI subclass codes — network (class 0x02)
#define PCI_SUBCLASS_ETHERNET  0x00

// PCI subclass codes — display (class 0x03)
#define PCI_SUBCLASS_VGA       0x00

// PCI subclass codes — bridge (class 0x06)
#define PCI_SUBCLASS_PCI_PCI   0x04

// PCI subclass codes — serial bus (class 0x0C)
#define PCI_SUBCLASS_USB       0x03

uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint8_t  pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);

void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);

uint16_t pci_check_vendor(uint8_t bus, uint8_t device, uint8_t function);
uint16_t pci_get_device_id(uint8_t bus, uint8_t device, uint8_t function);
uint8_t  pci_get_header_type(uint8_t bus, uint8_t device, uint8_t function);
uint8_t  pci_get_secondary_bus(uint8_t bus, uint8_t device, uint8_t function);

void check_function(uint8_t bus, uint8_t device, uint8_t function, uint8_t class_code, uint8_t subclass);
void check_device(uint8_t bus, uint8_t device);
void check_bus(uint8_t bus);
void check_all_buses(void);

#endif