use serial::serial_println;
use limine::response::MpResponse;

#[derive(Debug, Clone, Copy)]
enum CoreType {
    Performance,
    Efficiency,
    Standard,
    Unknown(u8),
}

impl CoreType {
    fn from_intel_native_model(value: u8) -> Self {
        match value {
            0x20 => CoreType::Efficiency,
            0x40 => CoreType::Performance,
            0x00 => CoreType::Standard,
            _    => CoreType::Unknown(value),
        }
    }

    fn as_str(&self) -> &'static str {
        match self {
            CoreType::Performance => "P-core (Performance)",
            CoreType::Efficiency  => "E-core (Efficiency)",
            CoreType::Standard    => "Standard SMP Core",
            CoreType::Unknown(_)  => "Unknown Core Type",
        }
    }
}

pub fn init_mp(res: &'static MpResponse) {
    serial_println!("Total CPU cores detected: {}", res.cpus().len());

    let hybrid_supported = check_hybrid_support();

    if hybrid_supported {
        serial_println!("Intel Hybrid Architecture detected (P-cores + E-cores)\n");
    } else {
        serial_println!("Standard SMP architecture\n");
    }

    let mut p_core_count = 0;
    let mut e_core_count = 0;
    let mut standard_count = 0;
    let mut unknown_count = 0;

    for (index, cpu) in res.cpus().iter().enumerate() {
        let core_type = if hybrid_supported {
            let core_type_raw = core::arch::x86_64::__cpuid(0x1A).eax >> 24;
            CoreType::from_intel_native_model(core_type_raw as u8)
        } else {
            CoreType::Standard
        };

        match core_type {
            CoreType::Performance => p_core_count += 1,
            CoreType::Efficiency  => e_core_count += 1,
            CoreType::Standard    => standard_count += 1,
            CoreType::Unknown(_)  => unknown_count += 1,
        }

        match core_type {
            CoreType::Unknown(val) => {
                serial_println!(
                    "  Core {}: LAPIC ID 0x{:X}, Type: Unknown (0x{:X})",
                    index, cpu.id, val
                );
            }
            _ => {
                serial_println!(
                    "  Core {}: LAPIC ID 0x{:X}, Type: {}",
                    index, cpu.id, core_type.as_str()
                );
            }
        }
    }

    serial_println!();
    serial_println!("Core Type Summary");
    if p_core_count > 0 {
        serial_println!("  P-cores (Performance): {}", p_core_count);
    }
    if e_core_count > 0 {
        serial_println!("  E-cores (Efficiency): {}", e_core_count);
    }
    if standard_count > 0 {
        serial_println!("  Standard SMP cores: {}", standard_count);
    }
    if unknown_count > 0 {
        serial_println!("  Unknown core types: {}", unknown_count);
    }
}

fn check_hybrid_support() -> bool {
    let max_leaf = core::arch::x86_64::__cpuid(0x0).eax;

    if max_leaf >= 0x1A {
        let features = core::arch::x86_64::__cpuid_count(0x07, 0x0);
        (features.edx & (1 << 15)) != 0
    } else {
        false
    }
}
