use limine::request::FramebufferRequest;
use framebuffer::println;
use memory::{addr::{PhysAddr, VirtAddr}, allocator::sbrk, paging::PageTableEntry, vmm::{Mapper, VMM}};

unsafe extern "C" {
    static FRAMEBUFFER_REQUEST: FramebufferRequest;
}

const ENOSYS: i64 = -38;
const EINVAL: i64 = -22;
const HHDM_OFFSET: u64 = 0xFFFF800000000000;
const USER_FB_VA:  u64 = 0x0000_7000_0000_0000;

#[repr(u64)]
pub enum Syscall {
    PlotPoint       = 0,
    GetFramebuffer  = 1,
    FramebufferInfo = 2,
    Print           = 3,
    TtyWrite        = 4,
    Sbrk            = 5,
}

impl Syscall {
    pub fn from_u64(n: u64) -> Option<Self> {
        match n {
            0 => Some(Self::PlotPoint),
            1 => Some(Self::GetFramebuffer),
            2 => Some(Self::FramebufferInfo),
            3 => Some(Self::Print),
            4 => Some(Self::TtyWrite),
            5 => Some(Self::Sbrk),
            _ => None,
        }
    }
}

pub struct SyscallHandler;

impl SyscallHandler {
    pub fn new() -> Self {
        Self
    }

    pub fn handle(&self, syscall_number: u64, arg1: i64, arg2: i64, arg3: i64) -> i64 {
        match Syscall::from_u64(syscall_number) {
            Some(Syscall::PlotPoint)       => self.plot_point(arg1, arg2, arg3),
            Some(Syscall::GetFramebuffer)  => self.handle_get_framebuffer(),
            Some(Syscall::FramebufferInfo) => self.framebuffer_info(arg1),
            Some(Syscall::Print)           => self.print(arg1, arg2),
            Some(Syscall::TtyWrite)        => self.tty_write(arg1, arg2),
            Some(Syscall::Sbrk)            => self.sbrk(arg1),
            None                           => ENOSYS,
        }
    }

    fn get_framebuffer(&self) -> Option<&'static limine::framebuffer::Framebuffer> {
        unsafe { FRAMEBUFFER_REQUEST.response() }?
            .framebuffers().first().copied()
    }

    fn handle_get_framebuffer(&self) -> i64 {
        let fb = match self.get_framebuffer() {
            Some(fb) => fb,
            None     => return 0,
        };
    
        let fb_virt  = fb.address() as u64;
        let fb_phys  = fb_virt - HHDM_OFFSET;
        let fb_size  = (fb.pitch as usize) * (fb.height as usize);
    
        let mapper = VMM::get_mapper();
        let pml4 = Mapper::get_current_page_table();
    
        let flags = PageTableEntry::PRESENT
                  | PageTableEntry::WRITABLE
                  | PageTableEntry::USER;
    
        match mapper.map_range(
            pml4,
            VirtAddr::new(USER_FB_VA),
            PhysAddr::new(fb_phys),
            fb_size,
            flags,
        ) {
            Some(_) => USER_FB_VA as i64,
            None    => 0,
        }
    }

    fn plot_point(&self, x: i64, y: i64, color: i64) -> i64 {
        if let Some(fb) = self.get_framebuffer() {
            if x < 0 || y < 0 || x >= fb.width as i64 || y >= fb.height as i64 {
                return EINVAL;
            }
            let offset = (y * fb.pitch as i64 + x * 4) as usize;
            unsafe {
                (fb.address() as *mut u8)
                    .add(offset)
                    .cast::<u32>()
                    .write(color as u32);
            }
        }
        0
    }

    fn framebuffer_info(&self, query: i64) -> i64 {
        if let Some(fb) = self.get_framebuffer() {
            match query {
                0 => fb.width as i64,
                1 => fb.height as i64,
                2 => fb.pitch as i64,
                3 => fb.bpp as i64,
                _ => 0,
            }
        } else {
            0
        }
    }

    fn print(&self, ptr: i64, len: i64) -> i64 {
        if !( ptr == 0 || len <= 0) {
            let slice = unsafe { core::slice::from_raw_parts(ptr as *const u8, len as usize) };
            if let Ok(s) = core::str::from_utf8(slice) {
                println!("{}", s);
            }
        }
        0
    }

    fn tty_write(&self, ptr: i64, len: i64) -> i64 {
        if ptr == 0 || len <= 0 || len > 65536 {
            return EINVAL;
        }
        let slice = unsafe { core::slice::from_raw_parts(ptr as *const u8, len as usize) };
        tty::tty_write(slice);
        len
    }

    fn sbrk(&self, increment: i64) -> i64 {
        let ptr = sbrk(increment as isize);
        if ptr.is_null() { -1 } else { ptr as i64 }
    }
}

pub extern "C" fn syscall_handler(syscall_number: u64, arg1: i64, arg2: i64, arg3: i64) -> i64 {
    SyscallHandler::new().handle(syscall_number, arg1, arg2, arg3)
}