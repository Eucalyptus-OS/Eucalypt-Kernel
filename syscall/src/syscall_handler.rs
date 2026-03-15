/// Object based syscall handler

use limine::request::FramebufferRequest;
use framebuffer::println;

unsafe extern "C" {
    static FRAMEBUFFER_REQUEST: FramebufferRequest;
}

#[repr(u64)]
pub enum Syscall {
    PlotPoint = 0,
    FramebufferInfo = 1,
    Print = 2,
}

impl Syscall {
    pub fn from_u64(n: u64) -> Option<Self> {
        match n {
            0 => Some(Self::PlotPoint),
            1 => Some(Self::FramebufferInfo),
            2 => Some(Self::Print),
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
            Some(Syscall::PlotPoint) => self.plot_point(arg1, arg2, arg3),
            Some(Syscall::FramebufferInfo) => self.framebuffer_info(arg1),
            Some(Syscall::Print) => self.print(arg1, arg2),
            None => 0xFFFFFFFFFFFFFFFFu64 as i64,
        }
    }

    fn plot_point(&self, x: i64, y: i64, color: i64) -> i64 {
        if let Some(fb) = self.get_framebuffer() {
            let pitch = fb.pitch() as i64;
            let offset = (y * pitch + x * 4) as usize;
            unsafe {
                fb.addr().add(offset).cast::<u32>().write(color as u32);
            }
        }
        0
    }

    fn framebuffer_info(&self, query: i64) -> i64 {
        if let Some(fb) = self.get_framebuffer() {
            match query {
                0 => fb.width() as i64,
                1 => fb.height() as i64,
                2 => fb.pitch() as i64,
                3 => fb.bpp() as i64,
                _ => 0,
            }
        } else {
            0
        }
    }

    fn print(&self, ptr: i64, len: i64) -> i64 {
        let ptr = ptr as *const u8;
        let len = len as usize;
        if !ptr.is_null() && len > 0 {
            let slice = unsafe { core::slice::from_raw_parts(ptr, len) };
            if let Ok(s) = core::str::from_utf8(slice) {
                println!("{}", s);
            }
        }
        0
    }

    fn get_framebuffer(&'_ self) -> Option<limine::framebuffer::Framebuffer<'_>> {
        unsafe { FRAMEBUFFER_REQUEST.get_response() }?
            .framebuffers()
            .next()
    }
}

pub extern "C" fn syscall_handler(syscall_number: u64, arg1: i64, arg2: i64, arg3: i64) -> i64 {
    SyscallHandler::new().handle(syscall_number, arg1, arg2, arg3)
}