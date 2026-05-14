#include <stdint.h>
#include <portio.h>
#include <logging/printk.h>
#include <drivers/pci.h>

// Read a 32-bit double word from PCI configuration space
uint32_t pci_config_read_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    // 31 Enable | 30-24 Reserved | 23-16 Bus Number |
    // 15-11 Device Number | 10-8 Function Number | 7-0 Register offset
    uint32_t address = ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
                       ((uint32_t)function << 8) | (offset & 0xFC) | 0x80000000;
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

// Read a 16-bit word from PCI configuration space
uint16_t pci_config_read_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t dword = pci_config_read_dword(bus, device, function, offset);
    return (uint16_t)((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

// Read a single byte from PCI configuration space
uint8_t pci_config_read_byte(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    // Read a word then extract the correct byte
    uint16_t word = pci_config_read_word(bus, device, function, offset);
    return (offset & 1) ? (word >> 8) : (word & 0xFF);
}

// Write a 32-bit double word to PCI configuration space
void pci_config_write_dword(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    // 31 Enable | 30-24 Reserved | 23-16 Bus Number |
    // 15-11 Device Number | 10-8 Function Number | 7-0 Register offset
    uint32_t address = ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
                       ((uint32_t)function << 8) | (offset & 0xFC) | 0x80000000;
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

// Write a 16-bit word to PCI configuration space
void pci_config_write_word(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    // Read the full dword first to preserve the other 16 bits
    uint32_t dword = pci_config_read_dword(bus, device, function, offset);
    uint8_t shift = (offset & 2) * 8;
    dword = (dword & ~(0xFFFFU << shift)) | ((uint32_t)value << shift);
    pci_config_write_dword(bus, device, function, offset, dword);
}

// Return the vendor ID for a given bus/device/function, or 0xFFFF if no device
uint16_t pci_check_vendor(uint8_t bus, uint8_t device, uint8_t function) {
    uint16_t vendor = pci_config_read_word(bus, device, function, PCI_OFFSET_VENDOR_ID);
    // If there is no device the vendor will be 0xFFFF
    if (vendor != PCI_VENDOR_NONE) {
        return vendor;
    }
    return PCI_VENDOR_NONE;
}

// Return the device ID for a given bus/device/function
uint16_t pci_get_device_id(uint8_t bus, uint8_t device, uint8_t function) {
    return pci_config_read_word(bus, device, function, PCI_OFFSET_DEVICE_ID);
}

// Return the header type byte for a given bus/device/function
uint8_t pci_get_header_type(uint8_t bus, uint8_t device, uint8_t function) {
    return pci_config_read_byte(bus, device, function, PCI_OFFSET_HEADER_TYPE);
}

// Return the secondary bus number exposed by a PCI-to-PCI bridge
uint8_t pci_get_secondary_bus(uint8_t bus, uint8_t device, uint8_t function) {
    // Secondary bus number lives at offset 0x19 in the bridge header
    return pci_config_read_byte(bus, device, function, PCI_OFFSET_SECONDARY_BUS);
}

// Inspect a single function and act on recognised class/subclass combinations
void check_function(uint8_t bus, uint8_t device, uint8_t function, uint8_t class_code, uint8_t subclass) {
    // AHCI controller
    if (class_code == PCI_CLASS_STORAGE && subclass == PCI_SUBCLASS_AHCI) {
        log_info("Found AHCI controller on bus %d, device %d, function %d\n", bus, device, function);
    }
    // IDE controller
    if (class_code == PCI_CLASS_STORAGE && subclass == PCI_SUBCLASS_IDE) {
        log_info("Found IDE controller on bus %d, device %d, function %d\n", bus, device, function);
    }
    // Network controller
    if (class_code == PCI_CLASS_NETWORK && subclass == PCI_SUBCLASS_ETHERNET) {
        log_info("Found network controller on bus %d, device %d, function %d\n", bus, device, function);
    }
    // VGA compatible display controller
    if (class_code == PCI_CLASS_DISPLAY && subclass == PCI_SUBCLASS_VGA) {
        log_info("Found VGA controller on bus %d, device %d, function %d\n", bus, device, function);
    }
    // USB controller
    if (class_code == PCI_CLASS_SERIAL && subclass == PCI_SUBCLASS_USB) {
        log_info("Found USB controller on bus %d, device %d, function %d\n", bus, device, function);
    }
    // PCI-to-PCI bridge: scan the secondary bus it exposes
    if (class_code == PCI_CLASS_BRIDGE && subclass == PCI_SUBCLASS_PCI_PCI) {
        uint8_t secondary_bus = pci_get_secondary_bus(bus, device, function);
        log_info("Found PCI bridge on bus %d, device %d, scanning secondary bus %d\n", bus, device, secondary_bus);
        check_bus(secondary_bus);
    }
}

// Enumerate all functions of a device and dispatch each to check_function
void check_device(uint8_t bus, uint8_t device) {
    uint16_t vendor = pci_check_vendor(bus, device, 0);
    // No device present
    if (vendor == PCI_VENDOR_NONE) return;

    uint8_t header_type = pci_get_header_type(bus, device, 0);
    uint8_t class_code  = pci_config_read_byte(bus, device, 0, PCI_OFFSET_CLASS_CODE);
    uint8_t subclass    = pci_config_read_byte(bus, device, 0, PCI_OFFSET_SUBCLASS);

    // Check function 0 regardless of multifunction status
    check_function(bus, device, 0, class_code, subclass);

    // Bit 7 of the header type indicates a multifunction device
    if (header_type & PCI_HEADER_MULTIFUNCTION) {
        uint8_t function;
        for (function = 1; function < 8; function++) {
            if (pci_check_vendor(bus, device, function) == PCI_VENDOR_NONE) continue;
            // Read class/subclass for this specific function, not function 0
            class_code = pci_config_read_byte(bus, device, function, PCI_OFFSET_CLASS_CODE);
            subclass   = pci_config_read_byte(bus, device, function, PCI_OFFSET_SUBCLASS);
            check_function(bus, device, function, class_code, subclass);
        }
    }
}

// Scan every possible device slot on a single bus
void check_bus(uint8_t bus) {
    uint8_t device;
    for (device = 0; device < 32; device++) {
        check_device(bus, device);
    }
}

// Enumerate all 256 buses and all 32 device slots on each
void check_all_buses(void) {
    uint16_t bus;
    for (bus = 0; bus < 256; bus++) {
        check_bus((uint8_t)bus);
    }
}