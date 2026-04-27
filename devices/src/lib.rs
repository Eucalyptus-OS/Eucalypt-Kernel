#![no_std]

use spin::Mutex;

pub mod event;
mod keyboard;
mod mouse;
mod ring_buffer;

pub use event::InputEvent;
pub use keyboard::KeyEvent;
pub use mouse::MouseEvent;
pub use ring_buffer::EventQueue;

pub static EVENT_QUEUE: EventQueue  = EventQueue::new();
pub static KEYBOARD: Mutex<Keyboard> = Mutex::new(Keyboard::new());
pub static MOUSE:    Mutex<Mouse>    = Mutex::new(Mouse::new());

pub struct Keyboard {
    prev_scan_code:    u8,
    current_scan_code: u8,
}

pub struct Mouse {
    packet:   [u8; 3],
    byte_idx: u8,
}

pub fn init_devices() {
    keyboard::init_keyboard();
    mouse::init_mouse();
}

impl Keyboard {
    pub const fn new() -> Self {
        Self { prev_scan_code: 0, current_scan_code: 0 }
    }

    pub fn read_and_update(&mut self) -> Option<KeyEvent> {
        let ke = keyboard::read_scan_code()?;
        self.prev_scan_code    = self.current_scan_code;
        self.current_scan_code = ke.scancode;
        Some(ke)
    }

    pub fn get_prev_scan_code(&self)    -> u8 { self.prev_scan_code    }
    pub fn get_current_scan_code(&self) -> u8 { self.current_scan_code }
}

impl Mouse {
    pub const fn new() -> Self {
        Self { packet: [0u8; 3], byte_idx: 0 }
    }

    pub fn handle_irq(&mut self) -> Option<MouseEvent> {
        mouse::handle_irq_byte(&mut self.packet, &mut self.byte_idx)
    }
}