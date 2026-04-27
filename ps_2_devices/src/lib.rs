#![no_std]

use spin::Mutex;

pub static KEYBOARD: Mutex<Keyboard> = Mutex::new(Keyboard::new());

pub struct Keyboard {
    prev_scan_code: u8,
    current_scan_code: u8,
}

pub struct Mouse {}

mod keyboard;

impl Keyboard {
    pub const fn new() -> Self {
        Self {
            prev_scan_code: 0,
            current_scan_code: 0,
        }
    }

    pub fn irq(&self) -> u8 {
        keyboard::read_scan_code().unwrap_or(0)
    }

    pub fn handle_scan_code(&mut self, scan_code: u8) {
        self.prev_scan_code = self.current_scan_code;
        self.current_scan_code = scan_code;
    }

    pub fn get_prev_scan_code(&self) -> u8 {
        self.prev_scan_code
    }

    pub fn get_current_scan_code(&self) -> u8 {
        self.current_scan_code
    }
}

impl Mouse {}