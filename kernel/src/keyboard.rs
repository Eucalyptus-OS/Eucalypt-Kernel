use core::sync::atomic::{AtomicBool, Ordering};
use bare_x86_64::inb;

const KB_DATA: u16 = 0x60;
const KB_STATUS: u16 = 0x64;

static SHIFT_DOWN: AtomicBool = AtomicBool::new(false);
static CAPS_LOCK: AtomicBool = AtomicBool::new(false);

#[rustfmt::skip]
static NORMAL: [u8; 58] = [
    0,
    0x1B,                                                          // Escape
    b'1', b'2', b'3', b'4', b'5', b'6', b'7', b'8', b'9', b'0',
    b'-', b'=',
    0x08,                                                          // Backspace
    b'\t',
    b'q', b'w', b'e', b'r', b't', b'y', b'u', b'i', b'o', b'p',
    b'[', b']',
    b'\r',                                                         // Enter
    0,                                                             // Left Ctrl
    b'a', b's', b'd', b'f', b'g', b'h', b'j', b'k', b'l',
    b';', b'\'', b'`',
    0,                                                             // Left Shift
    b'\\',
    b'z', b'x', b'c', b'v', b'b', b'n', b'm',
    b',', b'.', b'/',
    0,                                                             // Right Shift
    b'*', 0,                                                       // Keypad *, Left Alt
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

pub fn keyboard_irq_handler() {
    // Only read if the controller actually has data
    if inb!(KB_STATUS) & 0x01 == 0 {
        return;
    }

    let scancode = inb!(KB_DATA);
    let released = (scancode & 0x80) != 0;
    let code = scancode & 0x7F;

    match code {
        0x2A | 0x36 => {
            SHIFT_DOWN.store(!released, Ordering::Release);
            return;
        }
        0x3A => {
            if !released {
                let prev = CAPS_LOCK.load(Ordering::Acquire);
                CAPS_LOCK.store(!prev, Ordering::Release);
            }
            return;
        }
        _ => {}
    }

    if released { return; }

    let code = code as usize;
    if code >= NORMAL.len() { return; }

    let shift = SHIFT_DOWN.load(Ordering::Acquire);
    let caps  = CAPS_LOCK.load(Ordering::Acquire);

    let ch = if shift { SHIFTED[code] } else { NORMAL[code] };
    if ch == 0 { return; }

    let ch = if caps && ch.is_ascii_alphabetic() {
        if shift { ch.to_ascii_lowercase() } else { ch.to_ascii_uppercase() }
    } else {
        ch
    };

    tty::tty_handle_char(ch);
}
