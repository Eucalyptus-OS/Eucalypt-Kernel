use limine::request::FramebufferRequest;
use framebuffer::println;

unsafe extern "C" {
    static FRAMEBUFFER_REQUEST: FramebufferRequest;
}

const PLOT_POINT: u64 = 0;
const FRAMEBUFFER_INFO: u64 = 1;
const PRINT: u64 = 2;

pub fn syscall_handler(syscall_number: u64, _arg1: i64, _arg2: i64, _arg3: i64) -> i64 {
    match syscall_number {
        PLOT_POINT => {
            if let Some(framebuffer_response) = unsafe { FRAMEBUFFER_REQUEST.get_response() } {
                if let Some(framebuffer) = framebuffer_response.framebuffers().next() {
                    let x = _arg1;
                    let y = _arg2;
                    let color = _arg3 as u32;

                    let pitch = framebuffer.pitch() as u64;
                    let offset = (y * pitch as i64 + x * 4) as usize;

                    unsafe {
                        framebuffer.addr().add(offset).cast::<u32>().write(color);
                    }
                }
            }
            0
        },
        FRAMEBUFFER_INFO => {
            if let Some(framebuffer_response) = unsafe { FRAMEBUFFER_REQUEST.get_response() } {
                if let Some(framebuffer) = framebuffer_response.framebuffers().next() {
                    match _arg1 {
                        0 => framebuffer.width() as i64,
                        1 => framebuffer.height() as i64,
                        2 => framebuffer.pitch() as i64,
                        3 => framebuffer.bpp() as i64,
                        _ => 0,
                    }
                } else {
                    0
                }
            } else {
                0
            }
        }
    PRINT => {
        let ptr = _arg1 as *const u8;
        let len = _arg2 as usize;
        if !ptr.is_null() && len > 0 {
            let slice = unsafe { core::slice::from_raw_parts(ptr, len) };
            if let Ok(s) = core::str::from_utf8(slice) {
                println!("{}", s);
            }
        }
        0
    }
        _ => 0xFFFFFFFFFFFFFFFFu64 as i64,
    }
}