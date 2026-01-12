use crate::vmm;
use crate::addr;

static mut MMIO_LOWER: u64 = 0;     // Lower address for MMIO
static mut MMIO_UPPER: u64 = 0;     // Upper Address for MMIO
static mut MMIO_CURRENT: u64 = 0;   // Current allocation pointer

/// Sets the range for MMIO mappings
pub fn mmio_map_range(lower: u64, upper: u64) {
    unsafe {
        MMIO_LOWER = lower;
        MMIO_UPPER = upper;
        MMIO_CURRENT = lower;
    }
}

/// Maps a physical MMIO address to the predefined virtual MMIO region
pub fn map_mmio(addr: u64, size: u64) -> Result<u64, &'static str> {
    unsafe {
        // Align size to page boundary (4KB)
        let pages_needed = (size + 0xFFF) / 0x1000;
        let total_size = pages_needed * 0x1000;
        
        // Check if we have space
        if MMIO_CURRENT + total_size > MMIO_UPPER {
            return Err("MMIO region exhausted");
        }
        
        let virt_addr = MMIO_CURRENT;
        
        vmm::VMM::map_range(
            addr::VirtAddr::new(virt_addr),
            addr::PhysAddr::new(addr),
            vmm::PageTableEntry::WRITABLE | 
            vmm::PageTableEntry::NO_CACHE | 
            vmm::PageTableEntry::WRITE_THROUGH,
        ).expect("Failed to map MMIO region");
        
        MMIO_CURRENT += total_size;
        
        Ok(virt_addr)
    }
}

/// Returns the amount of MMIO space remaining
pub fn mmio_remaining() -> u64 {
    unsafe {
        MMIO_UPPER.saturating_sub(MMIO_CURRENT)
    }
}