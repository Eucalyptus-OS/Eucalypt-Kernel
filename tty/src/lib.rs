#![no_std]

use core::sync::atomic::{AtomicBool, Ordering};
use framebuffer::{color, fill_screen, write_global};

static INITIALIZED: AtomicBool = AtomicBool::new(false);

/// Initialize the TTY: clears the screen and marks the tty as ready.
pub fn tty_init() {
    fill_screen(color::BLACK);
    INITIALIZED.store(true, Ordering::Release);
}

/// Write raw bytes to the TTY. Silently drops output if called before tty_init.
pub fn tty_write(data: &[u8]) {
    if !INITIALIZED.load(Ordering::Acquire) {
        return;
    }
    write_global(data);
}

/// Convenience wrapper for writing a UTF-8 string.
pub fn tty_write_str(s: &str) {
    tty_write(s.as_bytes());
}
