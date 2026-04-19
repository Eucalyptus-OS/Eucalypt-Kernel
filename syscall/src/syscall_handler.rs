use limine::request::FramebufferRequest;
use framebuffer::println;
use memory::allocator::sbrk;

unsafe extern "C" {
    static FRAMEBUFFER_REQUEST: FramebufferRequest;
}

const ENOSYS: i64 = -38;
const EINVAL: i64 = -22;

#[repr(u64)]
pub enum Syscall {
    PlotPoint       = 0,
    FramebufferInfo = 1,
    Print           = 2,
    TtyWrite        = 3,
    Sbrk            = 4,
}

impl Syscall {
    pub fn from_u64(n: u64) -> Option<Self> {
        match n {
            0 => Some(Self::PlotPoint),
            1 => Some(Self::FramebufferInfo),
            2 => Some(Self::Print),
            3 => Some(Self::TtyWrite),
            4 => Some(Self::Sbrk),
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