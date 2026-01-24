use core::arch::asm;
use framebuffer::println;
use limine::request::MpRequest;

unsafe extern "C" {
    static MP_REQUEST: MpRequest;
}

pub fn init_mp() {
    let res = unsafe { MP_REQUEST.get_response() }.expect("No MP_REQUEST found!");
    
    for cpu in res.cpus().iter() {
        let core_type = unsafe { core::arch::x86_64::__cpuid(0x1A) }.eax >> 24;

        let type_str = match core_type {
            0x20 => "E-core (Efficiency)",
            0x40 => "P-core (Performance)",
            _    => "Standard SMP",
        };

        println!("CPU ID: 0x{:X}, Type: {}", cpu.id, type_str);
    }
    unsafe {asm!(
        "2:",
        "hlt",
        "jmp 2b"
    );}
}
