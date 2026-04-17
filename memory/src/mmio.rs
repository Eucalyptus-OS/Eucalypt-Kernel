use super::paging;
use super::vmm;
use super::addr;
use super::paging::PageTable;
use core::sync::atomic::{AtomicU64, Ordering};

// Lower bound of the MMIO virtual address window.
static MMIO_LOWER: AtomicU64 = AtomicU64::new(0);
// Exclusive upper bound of the MMIO virtual address window.
static MMIO_UPPER: AtomicU64 = AtomicU64::new(0);
// Next free virtual address within the MMIO window.
static MMIO_CURRENT: AtomicU64 = AtomicU64::new(0);

/// Initialises the MMIO virtual address window to [`lower`, `upper`).
pub fn mmio_map_range(lower: u64, upper: u64) {
    MMIO_LOWER.store(lower, Ordering::Release);
    MMIO_UPPER.store(upper, Ordering::Release);
    MMIO_CURRENT.store(lower, Ordering::Release);
}

/// Maps `size` bytes of MMIO starting at `phys_addr` into the next available slot in the MMIO window, returning the virtual address on success.
pub fn map_mmio(pml4: *mut PageTable, phys_addr: u64, size: u64) -> Result<u64, &'static str> {
    let pages_needed = (size + 0xFFF) / 0x1000;
    let total_size = pages_needed * 0x1000;

    let virt_addr = MMIO_CURRENT
        .fetch_update(Ordering::AcqRel, Ordering::Acquire, |current| {
            if current + total_size > MMIO_UPPER.load(Ordering::Acquire) {
                None
            } else {
                Some(current + total_size)
            }
        })
        .map_err(|_| "MMIO region exhausted")?;

    let mapper = vmm::VMM::get_mapper();

    for i in 0..pages_needed {
        let virt = addr::VirtAddr::new(virt_addr + (i * 0x1000));
        let phys = addr::PhysAddr::new(phys_addr + (i * 0x1000));

        mapper
            .map_page(
                pml4,
                virt,
                phys,
                paging::PageTableEntry::WRITABLE
                    | paging::PageTableEntry::NO_CACHE
                    | paging::PageTableEntry::WRITE_THROUGH,
            )
            .ok_or("Failed to map MMIO page")?;
    }

    Ok(virt_addr)
}

/// Returns the number of bytes remaining in the MMIO virtual address window.
pub fn mmio_remaining() -> u64 {
    MMIO_UPPER
        .load(Ordering::Acquire)
        .saturating_sub(MMIO_CURRENT.load(Ordering::Acquire))
}