use core::sync::atomic::{AtomicUsize, Ordering};
use limine::request::HhdmRequest;

#[used]
#[unsafe(no_mangle)]
#[unsafe(link_section = ".requests")]
static HHDM_REQUEST: HhdmRequest = HhdmRequest::new();

/// Runtime HHDM base set once by hhdm_init() during boot.
/// Falls back to the compile-time constant if Limine gives no response.
static HHDM_BASE: AtomicUsize = AtomicUsize::new(0xFFFF800000000000);

/// Must be called once during kmain before any phys_to_virt / virt_to_phys use.
pub fn hhdm_init() {
    if let Some(response) = HHDM_REQUEST.response() {
        HHDM_BASE.store(response.offset as usize, Ordering::Release);
    }
}

/// Returns the runtime HHDM base offset.
#[inline]
pub fn hhdm_offset() -> usize {
    HHDM_BASE.load(Ordering::Acquire)
}

/// Converts a physical address to a virtual address via the HHDM.
#[inline]
pub fn phys_to_virt(phys_addr: usize) -> usize {
    phys_addr + hhdm_offset()
}

/// Converts an HHDM virtual address back to its physical address.
#[inline]
pub fn virt_to_phys(virt_addr: usize) -> usize {
    virt_addr - hhdm_offset()
}