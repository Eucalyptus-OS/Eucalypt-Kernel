//! APIC support for x86_64 architecture
//! 
//! The APIC is the modern replacement for the obsolete PIT timer,
//! with better multi-core support and additional features.
//!
use super::cpu_types::CPUFeatures;
use super::msr::{read_msr, write_msr};
use core::arch::x86_64::__cpuid;
use core::sync::atomic::{AtomicUsize, Ordering};

const APIC_BASE_MSR: u32 = 0x1B;
const APIC_BASE_MSR_ENABLE: u64 = 0x800;
const APIC_SPURIOUS_INTERRUPT_VECTOR: usize = 0xF0;
const APIC_SOFTWARE_ENABLE: u32 = 0x100;
const APIC_TIMER_LVT: usize = 0x320;
const APIC_TIMER_INITIAL_COUNT: usize = 0x380;
const APIC_TIMER_CURRENT_COUNT: usize = 0x390;
const APIC_TIMER_DIVIDE_CONFIG: usize = 0x3E0;
const APIC_EOI: usize = 0xB0;

static APIC_VIRT_BASE: AtomicUsize = AtomicUsize::new(0);

#[unsafe(no_mangle)]
pub static mut APIC_TICKS_PER_SEC: u64 = 0;

fn set_apic_base(apic: usize) {
    let eax: u32 = ((apic & 0xfffff000) | APIC_BASE_MSR_ENABLE as usize) as u32;
    let edx: u32 = 0;
    write_msr(APIC_BASE_MSR, ((edx as u64) << 32) | (eax as u64));
}

/// Get the physical address of the APIC base
pub fn get_apic_base() -> usize {
    let msr_value: u64 = read_msr(APIC_BASE_MSR);
    (msr_value as usize) & 0xfffff000
}

/// Set the virtual address where the APIC is mapped
pub fn set_apic_virt_base(virt_addr: usize) {
    APIC_VIRT_BASE.store(virt_addr, Ordering::SeqCst);
}

/// Read from an APIC register at the given offset
fn read_apic_register(offset: usize) -> u32 {
    let apic_base = APIC_VIRT_BASE.load(Ordering::SeqCst);
    let register = (apic_base + offset) as *const u32;
    unsafe { core::ptr::read_volatile(register) }
}

/// Write to an APIC register at the given offset
fn write_apic_register(offset: usize, value: u32) {
    let apic_base = APIC_VIRT_BASE.load(Ordering::SeqCst);
    let register = (apic_base + offset) as *mut u32;
    unsafe { core::ptr::write_volatile(register, value) };
}

#[inline(always)]
fn rdtsc() -> u64 {
    let hi: u32;
    let lo: u32;
    unsafe {
        core::arch::asm!(
            "rdtsc",
            out("edx") hi,
            out("eax") lo,
            options(nomem, nostack, preserves_flags),
        );
    }
    ((hi as u64) << 32) | (lo as u64)
}

fn detect_tsc_frequency() -> u64 {
    let info = __cpuid(0x16);

    if info.eax != 0 {
        (info.eax as u64) * 1_000_000
    } else {
        3_000_000_000
    }
}

/// Enable the Local APIC
pub fn enable_apic() {
    let cpu_features = CPUFeatures::detect();
    if !cpu_features.apic {
        panic!("APIC not supported on this CPU");
    }
    set_apic_base(get_apic_base());
    let svr = read_apic_register(APIC_SPURIOUS_INTERRUPT_VECTOR);
    write_apic_register(APIC_SPURIOUS_INTERRUPT_VECTOR, svr | APIC_SOFTWARE_ENABLE);
}

/// Initialize the APIC timer
pub fn init_apic_timer(interrupt_vector: u8, initial_count: u32) {
    write_apic_register(APIC_TIMER_DIVIDE_CONFIG, 0x3);
    write_apic_register(APIC_TIMER_LVT, (interrupt_vector as u32) | (1 << 17));
    write_apic_register(APIC_TIMER_INITIAL_COUNT, initial_count);
}

/// Send End-Of-Interrupt signal to the APIC
pub fn apic_eoi() {
    write_apic_register(APIC_EOI, 0);
}

pub fn calibrate_apic_timer(target_hz: u64) -> u32 {
    const CALIBRATION_MS: u64 = 10;

    let tsc_start = rdtsc();

    write_apic_register(APIC_TIMER_DIVIDE_CONFIG, 0x3);
    write_apic_register(APIC_TIMER_LVT, 1 << 16);
    write_apic_register(APIC_TIMER_INITIAL_COUNT, u32::MAX);

    let tsc_freq = detect_tsc_frequency();
    let wait_cycles = tsc_freq * CALIBRATION_MS / 1000;
    while rdtsc() - tsc_start < wait_cycles {}

    let elapsed = u32::MAX - read_apic_register(APIC_TIMER_CURRENT_COUNT);

    let apic_freq = (elapsed as u64) * (1000 / CALIBRATION_MS);

    let initial = apic_freq / target_hz;

    unsafe {
        APIC_TICKS_PER_SEC = target_hz;
    }

    initial as u32
}
