use limine::request::FramebufferRequest;
use framebuffer::println;

unsafe extern "C" {
    static FRAMEBUFFER_REQUEST: FramebufferRequest;
}

// Syscall numbers
const PLOT_POINT:       u64 = 0;
const FRAMEBUFFER_INFO: u64 = 1;
const PRINT:            u64 = 2;

// Sentinel value returned for unknown syscalls
const SYSCALL_ERR: i64 = -1;

/// Retrieves the framebuffer from the Limine request.
/// Returns None if the response or framebuffer is unavailable.
fn get_framebuffer() -> Option<limine::framebuffer::Framebuffer<'static>> {
    unsafe { FRAMEBUFFER_REQUEST.get_response() }?
        .framebuffers()
        .next()
}

/// Plots a single pixel at (x, y) with the given color.
/// arg1 = x, arg2 = y, arg3 = color (u32 ARGB)
fn syscall_plot_point(x: i64, y: i64, color: i64) -> i64 {
    let Some(framebuffer) = get_framebuffer() else { return SYSCALL_ERR };
    let pitch = framebuffer.pitch() as i64;
    let offset = (y * pitch + x * 4) as usize;
    unsafe {
        framebuffer.addr().add(offset).cast::<u32>().write(color as u32);
    }
    0
}

/// Returns framebuffer info based on the requested field.
/// arg1: 0=width, 1=height, 2=pitch, 3=bpp
fn syscall_framebuffer_info(field: i64) -> i64 {
    let Some(framebuffer) = get_framebuffer() else { return SYSCALL_ERR };
    match field {
        0 => framebuffer.width() as i64,
        1 => framebuffer.height() as i64,
        2 => framebuffer.pitch() as i64,
        3 => framebuffer.bpp() as i64,
        _ => SYSCALL_ERR,
    }
}

/// Prints a UTF-8 string to the framebuffer console.
/// arg1 = pointer to string, arg2 = length
fn syscall_print(ptr: i64, len: i64) -> i64 {
    if ptr == 0 || len <= 0 {
        return SYSCALL_ERR;
    }
    let slice = unsafe { core::slice::from_raw_parts(ptr as *const u8, len as usize) };
    match core::str::from_utf8(slice) {
        Ok(s) => { println!("{}", s); 0 }
        Err(_) => SYSCALL_ERR,
    }
}

/// Main syscall dispatcher. Called from the syscall entry stub in idt.rs.
/// Convention: rdi=number, rsi=arg1, rdx=arg2, rcx=arg3
pub fn syscall_handler(syscall_number: u64, arg1: i64, arg2: i64, arg3: i64) -> i64 {
    match syscall_number {
        PLOT_POINT       => syscall_plot_point(arg1, arg2, arg3),
        FRAMEBUFFER_INFO => syscall_framebuffer_info(arg1),
        PRINT            => syscall_print(arg1, arg2),
        _                => SYSCALL_ERR,
    }
}