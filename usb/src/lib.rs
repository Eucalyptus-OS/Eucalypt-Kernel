#![no_std]

use framebuffer::println;
use pci::pci_find_xhci_controller;
use memory::mmio;

pub fn init_usb(addr: u64) {
    let virt;
    match mmio::map_mmio(addr, 0x4000) {
        Ok(xhci_virt) => {println!("XHCI virt addr: 0x{:X}", xhci_virt); virt = xhci_virt}
        Err(xhci_virt) => println!("Failed to map XHCI virt addr: {}", xhci_virt),
    }

}