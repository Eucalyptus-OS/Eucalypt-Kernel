#![no_std]

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use framebuffer::{color, fill_screen, write_global};

static INITIALIZED: AtomicBool = AtomicBool::new(false);

struct LineBuf(UnsafeCell<[u8; 256]>);
unsafe impl Sync for LineBuf {}
static LINE_BUF: LineBuf = LineBuf(UnsafeCell::new([0u8; 256]));
static LINE_LEN: AtomicUsize = AtomicUsize::new(0);

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

/// Called from the keyboard IRQ. Handles line editing and echoes to screen.
pub fn tty_handle_char(ch: u8) {
    if !INITIALIZED.load(Ordering::Acquire) {
        return;
    }
    match ch {
        b'\r' | b'\n' => {
            LINE_LEN.store(0, Ordering::Release);
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
