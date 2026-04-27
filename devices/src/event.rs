pub const MOD_SHIFT: u8 = 1 << 0;
pub const MOD_CAPS: u8 = 1 << 1;
pub const MOD_CTRL: u8 = 1 << 2;
pub const MOD_ALT: u8 = 1 << 3;

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum EventKind {
    None = 0,
    KeyPress = 1,
    KeyRelease = 2,
    MouseMove = 3,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct InputEvent {
    pub kind: u8,
    pub code: u8,
    pub scancode: u8,
    pub modifiers: u8,
    pub x: i16,
    pub y: i16,
    pub _pad: [u8; 8],
}

impl InputEvent {
    pub const fn zeroed() -> Self {
        Self {
            kind: 0,
            code: 0,
            scancode: 0,
            modifiers: 0,
            x: 0,
            y: 0,
            _pad: [0u8; 8],
        }
    }

    pub fn key(kind: EventKind, code: u8, scancode: u8, modifiers: u8) -> Self {
        Self {
            kind: kind as u8,
            code,
            scancode,
            modifiers,
            x: 0,
            y: 0,
            _pad: [0u8; 8],
        }
    }

    // buttons stored in code field (bit0=left, bit1=right, bit2=middle)
    pub fn mouse(dx: i16, dy: i16, buttons: u8) -> Self {
        Self {
            kind: EventKind::MouseMove as u8,
            code: buttons,
            scancode: 0,
            modifiers: 0,
            x: dx,
            y: dy,
            _pad: [0u8; 8],
        }
    }
}
