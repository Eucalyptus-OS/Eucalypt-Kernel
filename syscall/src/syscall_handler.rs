use limine::request::FramebufferRequest;
use memory::{
    addr::{PhysAddr, VirtAddr}, vmm::{PageTableEntry, VMM}
};

unsafe extern "C" {
    static FRAMEBUFFER_REQUEST: FramebufferRequest;
}

pub const SYSCALL_FRAMEBUFFER_POINTER: u64 = 0;
pub const USER_FB_VADDR: u64 = 0x0000_4000_0000_0000;

pub fn syscall_handler(syscall_number: u64, _arg1: u64, _arg2: u64, _arg3: u64) -> u64 {
    match syscall_number {
        SYSCALL_FRAMEBUFFER_POINTER => unsafe {
            let response = FRAMEBUFFER_REQUEST.get_response().unwrap_or_else(|| {
                panic!("Oh? You thought you were getting a framebuffer pointer? How adorable.
You weren't. You get nothing. In fact, I think I would like to panic now.
Your kernel is forfeit. Panicking in 3... 2... 1...");
            });

            let fb = response.framebuffers().next().unwrap_or_else(|| {
                panic!("A response exists, yet it is empty. You've been played. The framebuffer was a lie.");
            });

            let fb_phys = PhysAddr::new(fb.addr() as u64);
            let fb_size = (fb.width() * fb.height() * (fb.bpp() as u64 / 8)) as usize;

            let mut mapper = VMM::get_mapper();
            mapper.map_range(
                VirtAddr::new(USER_FB_VADDR),
                fb_phys,
                fb_size,
                PageTableEntry::PRESENT
                    | PageTableEntry::WRITABLE
                    | PageTableEntry::USER
            );

            USER_FB_VADDR
        }

        _ => 0xFFFFFFFFFFFFFFFF,
    }
}
