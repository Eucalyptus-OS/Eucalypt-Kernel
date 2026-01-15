use core::arch::x86_64::__cpuid;

#[macro_export]
macro_rules! define_cpu_features {
    (
        edx: [$(($edx_name:ident, $edx_bit:expr)),* $(,)?],
        ecx: [$(($ecx_name:ident, $ecx_bit:expr)),* $(,)?]
    ) => {
        pub struct CPUFeatures {
            $(pub $edx_name: bool,)*
            $(pub $ecx_name: bool,)*
        }

        impl CPUFeatures {
            pub fn detect() -> Self {
                let result = __cpuid(1);
                
                Self {
                    $($edx_name: (result.edx & (1 << $edx_bit)) != 0,)*
                    $($ecx_name: (result.ecx & (1 << $ecx_bit)) != 0,)*
                }
            }
        }
    };
}

define_cpu_features! {
    edx: [
        (fpu, 0),
        (vme, 1),
        (de, 2),
        (pse, 3),
        (tsc, 4),
        (msr, 5),
        (pae, 6),
        (mce, 7),
        (cx8, 8),
        (apic, 9),
        (sep, 11),
        (mtrr, 12),
        (pge, 13),
        (mca, 14),
        (cmov, 15),
        (pat, 16),
        (pse36, 17),
        (psn, 18),
        (clfsh, 19),
        (ds, 21),
        (acpi, 22),
        (mmx, 23),
        (fxsr, 24),
        (sse, 25),
        (sse2, 26),
        (ss, 27),
        (htt, 28),
        (tm, 29),
        (pbe, 31),
    ],
    ecx: [
        (sse3, 0),
        (pclmulqdq, 1),
        (monitor, 3),
        (vmx, 5),
        (ssse3, 9),
        (fma, 12),
        (cmpxchg16b, 13),
        (sse41, 19),
        (sse42, 20),
        (x2apic, 21),
        (movbe, 22),
        (popcnt, 23),
        (aes, 25),
        (xsave, 26),
        (osxsave, 27),
        (avx, 28),
        (f16c, 30),
        (rdrand, 30),
        (hypervisor, 31),
    ]
}