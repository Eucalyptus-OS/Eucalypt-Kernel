#![no_std]

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use framebuffer::{color, fill_screen, write_global};

static INITIALIZED: AtomicBool = AtomicBool::new(false);

struct LineBuf(UnsafeCell<[u8; 256]>);
unsafe impl Sync for LineBuf {}
static LINE_BUF: LineBuf = LineBuf(UnsafeCell::new([0u8; 256]));
static LINE_LEN: AtomicUsize = AtomicUsize::new(0);

static COOKED_BUF: LineBuf = LineBuf(UnsafeCell::new([0u8; 256]));
static COOKED_LEN: AtomicUsize = AtomicUsize::new(0);
static LINE_READY: AtomicBool = AtomicBool::new(false);

pub fn tty_init() {
    fill_screen(color::BLACK);
    INITIALIZED.store(true, Ordering::Release);
}

pub fn tty_write(data: &[u8]) {
    if !INITIALIZED.load(Ordering::Acquire) {
        return;
    }
    write_global(data);
}

pub fn tty_write_str(s: &str) {
    tty_write(s.as_bytes());
}

pub fn tty_handle_char(ch: u8) {
    if !INITIALIZED.load(Ordering::Acquire) {
        return;
    }
    match ch {
        b'\r' | b'\n' => {
            let len = LINE_LEN.load(Ordering::Acquire);
            // copy editing buf → cooked buf
            unsafe {
                let src = &*LINE_BUF.0.get();
                let dst = &mut *COOKED_BUF.0.get();
                dst[..len].copy_from_slice(&src[..len]);
            }
            COOKED_LEN.store(len, Ordering::Release);
            LINE_LEN.store(0, Ordering::Release);
            LINE_READY.store(true, Ordering::Release);
            write_global(b"\n> ");
        }
        0x08 | 0x7F => {
            let len = LINE_LEN.load(Ordering::Acquire);
            if len > 0 {
                LINE_LEN.store(len - 1, Ordering::Release);
                write_global(b"\x08");
            }
        }
        0x20..=0x7E => {
            let len = LINE_LEN.load(Ordering::Acquire);
            if len < 255 {
                unsafe { (*LINE_BUF.0.get())[len] = ch; }
                LINE_LEN.store(len + 1, Ordering::Release);
                write_global(core::slice::from_ref(&ch));
            }
        }
        _ => {}
    }
}

/// Blocks (spinning with `hlt`) until the user presses Enter, then copies the
/// completed line into `buf` (without the newline) and returns the byte count.
/// Returns 0 if `buf` is empty or the TTY is not yet initialized.
pub fn tty_read_line(buf: &mut [u8]) -> usize {
    if buf.is_empty() || !INITIALIZED.load(Ordering::Acquire) {
        return 0;
    }

    // spin until Enter was pressed
    while !LINE_READY.load(Ordering::Acquire) {
        unsafe { core::arch::asm!("hlt") };
    }

    let len = COOKED_LEN.load(Ordering::Acquire).min(buf.len());
    unsafe {
        buf[..len].copy_from_slice(&(&(*COOKED_BUF.0.get()))[..len]);
    }
    LINE_READY.store(false, Ordering::Release);
    len
}
