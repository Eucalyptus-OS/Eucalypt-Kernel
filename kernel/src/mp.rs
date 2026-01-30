use core::arch::asm;
use framebuffer::println;
use limine::request::MpRequest;

unsafe extern "C" {
    static MP_REQUEST: MpRequest;
}

#[derive(Debug, Clone, Copy)]
enum CoreType {
    Performance,      // P-core (0x40)
    Efficiency,       // E-core (0x20)
    Standard,         // Regular symmetric core (0x00)
    Unknown(u8),      // Future/unknown types
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
            CoreType::Efficiency => "E-core (Efficiency)",
            CoreType::Standard => "Standard SMP Core",
            CoreType::Unknown(val) => {
                "Unknown Core Type"
            }
        }
    }
}

pub fn init_mp() {
    let res = unsafe { MP_REQUEST.get_response() }.expect("No MP_REQUEST found!");
    println!("Total CPU cores detected: {}", res.cpus().len());
    
    let hybrid_supported = check_hybrid_support();
    
    if hybrid_supported {
        println!("Intel Hybrid Architecture detected (P-cores + E-cores)");
    } else {
        println!("Standard SMP architecture");
    }
    
    println!();

    let mut p_core_count = 0;
    let mut e_core_count = 0;
    let mut standard_count = 0;
    let mut unknown_count = 0;

    for (index, cpu) in res.cpus().iter().enumerate() {
        let core_type = if hybrid_supported {
            // CPUID.1Ah.EAX[31:24] contains the native model ID (core type)
            let core_type_raw = unsafe { 
                core::arch::x86_64::__cpuid(0x1A).eax >> 24 
            } as u8;
            CoreType::from_intel_native_model(core_type_raw)
        } else {
            CoreType::Standard
        };

        // Count by type
        match core_type {
            CoreType::Performance => p_core_count += 1,
            CoreType::Efficiency => e_core_count += 1,
            CoreType::Standard => standard_count += 1,
            CoreType::Unknown(_) => unknown_count += 1,
        }

        // Display core information
        match core_type {
            CoreType::Unknown(val) => {
                println!("  Core {}: LAPIC ID 0x{:X}, Type: Unknown (0x{:X})", 
                    index, cpu.id, val);
            }
            _ => {
                println!("  Core {}: LAPIC ID 0x{:X}, Type: {}", 
                    index, cpu.id, core_type.as_str());
            }
        }
    }

    println!();
    println!("Core Type Summary");
    if p_core_count > 0 {
        println!("  P-cores (Performance): {}", p_core_count);
    }
    if e_core_count > 0 {
        println!("  E-cores (Efficiency): {}", e_core_count);
    }
    if standard_count > 0 {
        println!("  Standard SMP cores: {}", standard_count);
    }
    if unknown_count > 0 {
        println!("  Unknown core types: {}", unknown_count);
    }
}

fn check_hybrid_support() -> bool {
    unsafe {
        // First check if CPUID supports leaf 0x1A
        let max_leaf = core::arch::x86_64::__cpuid(0x0).eax;
        
        if max_leaf >= 0x1A {
            let features = core::arch::x86_64::__cpuid_count(0x07, 0x0);
            (features.edx & (1 << 15)) != 0
        } else {
            false
        }
    }
}