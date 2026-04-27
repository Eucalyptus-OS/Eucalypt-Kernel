use crate::event::{MOD_ALT, MOD_CAPS, MOD_CTRL, MOD_SHIFT};
use bare_x86_64::{inb, outb};
use core::sync::atomic::{AtomicBool, Ordering};

const KB_DATA: u16 = 0x60;
const KB_STATUS: u16 = 0x64;

static SHIFT_DOWN: AtomicBool = AtomicBool::new(false);
static CAPS_LOCK: AtomicBool  = AtomicBool::new(false);
static CTRL_DOWN: AtomicBool  = AtomicBool::new(false);
static ALT_DOWN: AtomicBool   = AtomicBool::new(false);

pub struct KeyEvent {
    pub released:  bool,
    pub ch:        u8,
    pub scancode:  u8,
    pub modifiers: u8,
}

#[rustfmt::skip]
static NORMAL: [u8; 58] = [
    0,
    0x1B,
    b'1', b'2', b'3', b'4', b'5', b'6', b'7', b'8', b'9', b'0',
    b'-', b'=',
    0x08,
    b'\t',
    b'q', b'w', b'e', b'r', b't', b'y', b'u', b'i', b'o', b'p',
    b'[', b']',
    b'\r',
    0,
    b'a', b's', b'd', b'f', b'g', b'h', b'j', b'k', b'l',
    b';', b'\'', b'`',
    0,
    b'\\',
    b'z', b'x', b'c', b'v', b'b', b'n', b'm',
    b',', b'.', b'/',
    0,
    b'*', 0,
    b' ',
];

#[rustfmt::skip]
static SHIFTED: [u8; 58] = [
    0,
    0x1B,
    b'!', b'@', b'#', b'$', b'%', b'^', b'&', b'*', b'(', b')',
    b'_', b'+',
    0x08,
    b'\t',
    b'Q', b'W', b'E', b'R', b'T', b'Y', b'U', b'I', b'O', b'P',
    b'{', b'}',
    b'\r',
    0,
    b'A', b'S', b'D', b'F', b'G', b'H', b'J', b'K', b'L',
    b':', b'"', b'~',
    0,
    b'|',
    b'Z', b'X', b'C', b'V', b'B', b'N', b'M',
    b'<', b'>', b'?',
    0,
    b'*', 0,
    b' ',
];

pub fn init_keyboard() {
    unsafe { core::arch::asm!("cli"); }

    while inb!(0x64) & 0x01 != 0 { inb!(0x60); }
    while inb!(0x64) & 0x02 != 0 {}
    outb!(0x64, 0xAD);
    while inb!(0x64) & 0x02 != 0 {}
    outb!(0x64, 0xA7);
    while inb!(0x64) & 0x01 != 0 { inb!(0x60); }

    while inb!(0x64) & 0x02 != 0 {}
    outb!(0x64, 0x20);
    while inb!(0x64) & 0x01 == 0 {}
    let cfg = (inb!(0x60) | 0x01) & !0x10;
    while inb!(0x64) & 0x02 != 0 {}
    outb!(0x64, 0x60);
    while inb!(0x64) & 0x02 != 0 {}
    outb!(0x60, cfg);

    while inb!(0x64) & 0x02 != 0 {}
    outb!(0x64, 0xAE);
    while inb!(0x64) & 0x02 != 0 {}
    outb!(0x60, 0xF4);
    while inb!(0x64) & 0x01 == 0 {}
    inb!(0x60);

    while inb!(0x64) & 0x02 != 0 {}
    outb!(0x64, 0xA8);

    unsafe { core::arch::asm!("sti"); }
}

pub fn read_scan_code() -> Option<KeyEvent> {
    if inb!(KB_STATUS) & 0x01 == 0 {
        return None;
    }

    let raw = inb!(KB_DATA);
    let released = (raw & 0x80) != 0;
    let code = raw & 0x7F;

    match code {
        0x2A | 0x36 => { SHIFT_DOWN.store(!released, Ordering::Release); return None; }
        0x3A => {
            if !released {
                let prev = CAPS_LOCK.load(Ordering::Acquire);
                CAPS_LOCK.store(!prev, Ordering::Release);
            }
            return None;
        }
        0x1D => { CTRL_DOWN.store(!released, Ordering::Release); return None; }
        0x38 => { ALT_DOWN.store(!released,  Ordering::Release); return None; }
        _ => {}
    }

    let shift = SHIFT_DOWN.load(Ordering::Acquire);
    let caps  = CAPS_LOCK.load(Ordering::Acquire);

    let ch = if (code as usize) < NORMAL.len() {
        let c = if shift { SHIFTED[code as usize] } else { NORMAL[code as usize] };
        if c != 0 && caps && c.is_ascii_alphabetic() {
            if shift { c.to_ascii_lowercase() } else { c.to_ascii_uppercase() }
        } else {
            c
        }
    } else {
        0
    };

    let modifiers = {
        let mut m = 0u8;
        if shift                              { m |= MOD_SHIFT; }
        if caps                               { m |= MOD_CAPS;  }
        if CTRL_DOWN.load(Ordering::Acquire) { m |= MOD_CTRL;  }
        if ALT_DOWN.load(Ordering::Acquire)  { m |= MOD_ALT;   }
        m
    };

    Some(KeyEvent { released, ch, scancode: code, modifiers })
}