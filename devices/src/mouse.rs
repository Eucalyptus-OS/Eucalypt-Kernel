use bare_x86_64::{inb, outb};

const DATA: u16 = 0x60;
const CMD:  u16 = 0x64;

pub struct MouseEvent {
    pub dx:      i16,
    pub dy:      i16,
    pub buttons: u8,
}

fn wait_write() { while inb!(CMD) & 0x02 != 0 {} }
fn wait_read_byte() -> u8 { while inb!(CMD) & 0x01 == 0 {} inb!(DATA) }

fn mouse_write(b: u8) {
    wait_write(); outb!(CMD, 0xD4);
    wait_write(); outb!(DATA, b);
}

fn mouse_cmd(b: u8) -> u8 {
    mouse_write(b);
    wait_read_byte()
}

pub fn init_mouse() {
    unsafe { core::arch::asm!("cli"); }

    while inb!(CMD) & 0x01 != 0 { inb!(DATA); }
    wait_write(); outb!(CMD, 0xAD);
    wait_write(); outb!(CMD, 0xA7);
    while inb!(CMD) & 0x01 != 0 { inb!(DATA); }
    wait_write(); outb!(CMD, 0x20);
    let cfg = (wait_read_byte() | 0x02) & !0x20;
    wait_write(); outb!(CMD, 0x60);
    wait_write(); outb!(DATA, cfg);
    wait_write(); outb!(CMD, 0xA8);
    mouse_cmd(0xF6);
    mouse_cmd(0xF4);
    wait_write(); outb!(CMD, 0xAE);

    unsafe { core::arch::asm!("sti"); }
}

pub fn handle_irq_byte(buf: &mut [u8; 3], idx: &mut u8) -> Option<MouseEvent> {
    let byte = inb!(DATA);
    if *idx == 0 && byte & 0x08 == 0 {
        return None;
    }
    buf[*idx as usize] = byte;
    *idx += 1;
    if *idx < 3 {
        return None;
    }
    *idx = 0;
    let status = buf[0];
    if status & 0xC0 != 0 {
        return None;
    }
    let dx = if status & 0x10 != 0 { buf[1] as i16 - 256 } else { buf[1] as i16 };
    let dy = if status & 0x20 != 0 { buf[2] as i16 - 256 } else { buf[2] as i16 };
    Some(MouseEvent { dx, dy: -dy, buttons: status & 0x07 })
}