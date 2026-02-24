use crate::addr;
use crate::vmm;

static mut MMIO_LOWER: u64 = 0;
static mut MMIO_UPPER: u64 = 0;
static mut MMIO_CURRENT: u64 = 0;

pub fn mmio_map_range(lower: u64, upper: u64) {
    unsafe {
        MMIO_LOWER = lower;
        MMIO_UPPER = upper;
        MMIO_CURRENT = lower;
    }
}

pub fn map_mmio(phys_addr: u64, size: u64) -> Result<u64, &'static str> {
    unsafe {
        let pages_needed = (size + 0xFFF) / 0x1000;
        let total_size = pages_needed * 0x1000;

        if MMIO_CURRENT + total_size > MMIO_UPPER {
            return Err("MMIO region exhausted");
        }

        let virt_addr = MMIO_CURRENT;
        let mut mapper = vmm::VMM::get_mapper();

        for i in 0..pages_needed {
            let virt = addr::VirtAddr::new(virt_addr + (i * 0x1000));
            let phys = addr::PhysAddr::new(phys_addr + (i * 0x1000));

            mapper
                .map_page(
                    virt,
                    phys,
                    vmm::PageTableEntry::WRITABLE
                        | vmm::PageTableEntry::NO_CACHE
                        | vmm::PageTableEntry::WRITE_THROUGH,
                )
                .ok_or("Failed to map MMIO page")?;
        }

        MMIO_CURRENT += total_size;

        Ok(virt_addr)
    }
}

pub fn mmio_remaining() -> u64 {
    unsafe { MMIO_UPPER.saturating_sub(MMIO_CURRENT) }
}
