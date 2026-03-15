#![no_std]
//! IDE/ATA driver for x86
//! In 1986 Western Digital and Compaq created the ATA drive standard
//! also known as IDE, replacing older storage interfaces
extern crate alloc;

use bare_x86_64::*;
use core::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use framebuffer::println;

const ATA_SR_BSY: u8 = 0x80;
const ATA_SR_DF: u8 = 0x20;
const ATA_SR_DRQ: u8 = 0x08;
const ATA_SR_ERR: u8 = 0x01;

const ATA_ER_BBK: u8 = 0x80;
const ATA_ER_UNC: u8 = 0x40;
const ATA_ER_MC: u8 = 0x20;
const ATA_ER_IDNF: u8 = 0x10;
const ATA_ER_MCR: u8 = 0x08;
const ATA_ER_ABRT: u8 = 0x04;
const ATA_ER_TK0NF: u8 = 0x02;
const ATA_ER_AMNF: u8 = 0x01;

const ATA_CMD_READ_PIO: u8 = 0x20;
const ATA_CMD_READ_PIO_EXT: u8 = 0x24;
const ATA_CMD_WRITE_PIO: u8 = 0x30;
const ATA_CMD_WRITE_PIO_EXT: u8 = 0x34;
const ATA_CMD_CACHE_FLUSH: u8 = 0xE7;
const ATA_CMD_CACHE_FLUSH_EXT: u8 = 0xEA;
const ATA_CMD_IDENTIFY: u8 = 0xEC;

const ATA_IDENT_COMMANDSETS: usize = 164;
const ATA_IDENT_MAX_LBA: usize = 120;
const ATA_IDENT_MAX_LBA_EXT: usize = 200;
const ATA_IDENT_MODEL: usize = 54;

const ATA_REG_DATA: u16 = 0x00;
const ATA_REG_ERROR: u16 = 0x01;
const ATA_REG_SECCOUNT0: u16 = 0x02;
const ATA_REG_LBA0: u16 = 0x03;
const ATA_REG_LBA1: u16 = 0x04;
const ATA_REG_LBA2: u16 = 0x05;
const ATA_REG_HDDEVSEL: u16 = 0x06;
const ATA_REG_COMMAND: u16 = 0x07;
const ATA_REG_STATUS: u16 = 0x07;

const ATA_PRIMARY_BASE: u16 = 0x1F0;
const ATA_PRIMARY_CTRL: u16 = 0x3F6;
const ATA_SECONDARY_BASE: u16 = 0x170;
const ATA_SECONDARY_CTRL: u16 = 0x376;

const ATA_PRIMARY: usize = 0;
const ATA_SECONDARY: usize = 1;

const SECTOR_SIZE: usize = 512;
const QUADS_PER_SECTOR: u32 = 128;
const MAX_SECTORS_PER_TRANSFER: usize = 128;
const IRQ_TIMEOUT: u32 = 10_000_000;
const POLL_TIMEOUT: u32 = 100_000;

static IDE_LOCK: AtomicBool = AtomicBool::new(false);
static IDE_PRIMARY_IRQ: AtomicU8 = AtomicU8::new(0);
static IDE_SECONDARY_IRQ: AtomicU8 = AtomicU8::new(0);

struct IdeChannel {
    base: u16,
    ctrl: u16,
    bmide: u16,
}

static mut CHANNELS: [IdeChannel; 2] = [
    IdeChannel { base: ATA_PRIMARY_BASE,   ctrl: ATA_PRIMARY_CTRL,   bmide: 0 },
    IdeChannel { base: ATA_SECONDARY_BASE, ctrl: ATA_SECONDARY_CTRL, bmide: 0 },
];

#[repr(C)]
#[derive(Clone, Copy)]
pub struct IdeDevice {
    pub reserved: u8,
    pub channel: usize,
    pub drive: u8,
    pub size: u64,
    pub model: [u8; 41],
    pub lba48: bool,
}

impl IdeDevice {
    const fn zeroed() -> Self {
        Self { reserved: 0, channel: 0, drive: 0, size: 0, model: [0; 41], lba48: false }
    }
}

pub static mut IDE_DEVICES: [IdeDevice; 4] = [
    IdeDevice::zeroed(), IdeDevice::zeroed(),
    IdeDevice::zeroed(), IdeDevice::zeroed(),
];
pub static mut COUNT: usize = 0;

fn ide_lock() {
    while IDE_LOCK
        .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

fn ide_unlock() {
    IDE_LOCK.store(false, Ordering::Release);
}

fn base(channel: usize) -> u16 { unsafe { CHANNELS[channel].base } }
fn ctrl(channel: usize) -> u16 { unsafe { CHANNELS[channel].ctrl } }

fn ata_read(channel: usize, reg: u16) -> u8 {
    inb!(base(channel) + reg)
}

fn ata_write(channel: usize, reg: u16, val: u8) {
    outb!(base(channel) + reg, val)
}

fn ata_read_ctrl(channel: usize) -> u8 {
    inb!(ctrl(channel))
}

fn ata_write_ctrl(channel: usize, val: u8) {
    outb!(ctrl(channel), val)
}

fn ata_read_data_u32(channel: usize) -> u32 {
    inl!(base(channel) + ATA_REG_DATA)
}

fn ata_write_data_u32(channel: usize, val: u32) {
    outl!(base(channel) + ATA_REG_DATA, val)
}

// 400ns delay by reading alt-status 4 times
fn delay400ns(channel: usize) {
    for _ in 0..4 { ata_read_ctrl(channel); }
}

/// Sets or clears the HOB bit in the control register for LBA48 high-byte access.
fn set_hob(channel: usize, hob: bool) {
    ata_write_ctrl(channel, if hob { 0x80 } else { 0x00 });
}

/// Polls alt-status until BSY clears. Returns false on timeout.
fn poll_bsy(channel: usize) -> bool {
    let mut t = POLL_TIMEOUT;
    loop {
        if ata_read_ctrl(channel) & ATA_SR_BSY == 0 { return true; }
        t -= 1;
        if t == 0 { return false; }
        core::hint::spin_loop();
    }
}

/// Polls alt-status until BSY clears and DRQ sets. Returns false on timeout or error.
fn poll_drq(channel: usize) -> bool {
    let mut t = POLL_TIMEOUT;
    loop {
        let s = ata_read_ctrl(channel);
        if s & ATA_SR_ERR != 0 || s & ATA_SR_DF != 0 { return false; }
        if s & ATA_SR_BSY == 0 && s & ATA_SR_DRQ != 0 { return true; }
        t -= 1;
        if t == 0 { return false; }
        core::hint::spin_loop();
    }
}

/// Spins on the IRQ flag for the given channel. Returns false on timeout.
fn wait_irq(channel: usize) -> bool {
    let flag = if channel == ATA_PRIMARY { &IDE_PRIMARY_IRQ } else { &IDE_SECONDARY_IRQ };
    let mut t = IRQ_TIMEOUT;
    loop {
        if flag.swap(0, Ordering::AcqRel) != 0 { return true; }
        t -= 1;
        if t == 0 { return false; }
        core::hint::spin_loop();
    }
}

fn clear_irq(channel: usize) {
    if channel == ATA_PRIMARY {
        IDE_PRIMARY_IRQ.store(0, Ordering::Release);
    } else {
        IDE_SECONDARY_IRQ.store(0, Ordering::Release);
    }
}

/// Selects the drive on the channel and waits for BSY to clear.
fn select_drive(channel: usize, drive: u8, lba_high_nibble: u8, lba48: bool) {
    let sel = if lba48 {
        0x40 | (drive << 4)
    } else {
        0xE0 | (drive << 4) | (lba_high_nibble & 0x0F)
    };
    ata_write(channel, ATA_REG_HDDEVSEL, sel);
    delay400ns(channel);
    poll_bsy(channel);
}

/// Issues a PIO read or write command with full LBA28/LBA48 register setup.
fn issue_command(channel: usize, drive: u8, lba: u64, sectors: usize, lba48: bool, write: bool) {
    select_drive(channel, drive, ((lba >> 24) & 0x0F) as u8, lba48);

    if lba48 {
        // high bytes first (HOB=1), then low bytes (HOB=0)
        set_hob(channel, true);
        ata_write(channel, ATA_REG_SECCOUNT0, ((sectors >> 8) & 0xFF) as u8);
        ata_write(channel, ATA_REG_LBA0, ((lba >> 24) & 0xFF) as u8);
        ata_write(channel, ATA_REG_LBA1, ((lba >> 32) & 0xFF) as u8);
        ata_write(channel, ATA_REG_LBA2, ((lba >> 40) & 0xFF) as u8);
        set_hob(channel, false);
        ata_write(channel, ATA_REG_SECCOUNT0, (sectors & 0xFF) as u8);
        ata_write(channel, ATA_REG_LBA0, (lba & 0xFF) as u8);
        ata_write(channel, ATA_REG_LBA1, ((lba >> 8) & 0xFF) as u8);
        ata_write(channel, ATA_REG_LBA2, ((lba >> 16) & 0xFF) as u8);
        ata_write(channel, ATA_REG_COMMAND,
            if write { ATA_CMD_WRITE_PIO_EXT } else { ATA_CMD_READ_PIO_EXT });
    } else {
        ata_write(channel, ATA_REG_SECCOUNT0, (sectors & 0xFF) as u8);
        ata_write(channel, ATA_REG_LBA0, (lba & 0xFF) as u8);
        ata_write(channel, ATA_REG_LBA1, ((lba >> 8) & 0xFF) as u8);
        ata_write(channel, ATA_REG_LBA2, ((lba >> 16) & 0xFF) as u8);
        ata_write(channel, ATA_REG_COMMAND,
            if write { ATA_CMD_WRITE_PIO } else { ATA_CMD_READ_PIO });
    }
}

fn read_sector(channel: usize, buf: &mut [u8; SECTOR_SIZE]) {
    for i in 0..QUADS_PER_SECTOR as usize {
        let val = ata_read_data_u32(channel);
        buf[i * 4]     = (val & 0xFF) as u8;
        buf[i * 4 + 1] = ((val >> 8) & 0xFF) as u8;
        buf[i * 4 + 2] = ((val >> 16) & 0xFF) as u8;
        buf[i * 4 + 3] = ((val >> 24) & 0xFF) as u8;
    }
}

fn write_sector(channel: usize, buf: &[u8; SECTOR_SIZE]) {
    for i in 0..QUADS_PER_SECTOR as usize {
        let val = (buf[i * 4] as u32)
            | ((buf[i * 4 + 1] as u32) << 8)
            | ((buf[i * 4 + 2] as u32) << 16)
            | ((buf[i * 4 + 3] as u32) << 24);
        ata_write_data_u32(channel, val);
    }
}

fn print_error(drive: usize, err: u8) -> u8 {
    if err == 0 { return 0; }
    println!("IDE:");
    let mut code = err;
    if err == 1 {
        println!("- Device Fault");
        code = 19;
    } else if err == 2 {
        let st = ata_read(unsafe { IDE_DEVICES[drive].channel }, ATA_REG_ERROR);
        if st & ATA_ER_AMNF  != 0 { println!("- No Address Mark Found");    code = 7;  }
        if st & ATA_ER_TK0NF != 0 { println!("- No Media or Media Error");  code = 3;  }
        if st & ATA_ER_ABRT  != 0 { println!("- Command Aborted");          code = 20; }
        if st & ATA_ER_MCR   != 0 { println!("- No Media or Media Error");  code = 3;  }
        if st & ATA_ER_IDNF  != 0 { println!("- ID Mark Not Found");        code = 21; }
        if st & ATA_ER_MC    != 0 { println!("- No Media or Media Error");  code = 3;  }
        if st & ATA_ER_UNC   != 0 { println!("- Uncorrectable Data Error"); code = 22; }
        if st & ATA_ER_BBK   != 0 { println!("- Bad Sectors");              code = 13; }
    } else if err == 3 {
        println!("- Timeout / No Response");
        code = 23;
    } else if err == 4 {
        println!("- Write Protected");
        code = 8;
    }
    unsafe {
        let dev = &IDE_DEVICES[drive];
        println!("- [{} {}] {}",
            if dev.channel == ATA_PRIMARY { "Primary" } else { "Secondary" },
            if dev.drive == 0 { "Master" } else { "Slave" },
            core::str::from_utf8(&dev.model).unwrap_or("Unknown"));
    }
    code
}

pub fn ide_primary_irq_handler() {
    let status = ata_read(ATA_PRIMARY, ATA_REG_STATUS);
    if status & ATA_SR_BSY == 0 {
        IDE_PRIMARY_IRQ.store(1, Ordering::Release);
    }
}

pub fn ide_secondary_irq_handler() {
    let status = ata_read(ATA_SECONDARY, ATA_REG_STATUS);
    if status & ATA_SR_BSY == 0 {
        IDE_SECONDARY_IRQ.store(1, Ordering::Release);
    }
}

/// Reads `buffer.len()` bytes starting at `lba` from the given drive.
/// Uses IRQ-driven flow per sector.
pub fn ide_read_sectors(drive: usize, lba: u64, buffer: &mut [u8]) -> u8 {
    ide_lock();
    let result = unsafe {
        let dev = &IDE_DEVICES[drive];
        if dev.reserved == 0 { ide_unlock(); return 1; }

        let channel  = dev.channel;
        let drive_bit = dev.drive;
        let lba48    = dev.lba48 || lba >= 0x10000000;
        let total    = buffer.len() / SECTOR_SIZE;
        let mut done = 0usize;

        while done < total {
            let count = core::cmp::min(MAX_SECTORS_PER_TRANSFER, total - done);
            clear_irq(channel);
            issue_command(channel, drive_bit, lba + done as u64, count, lba48, false);

            for s in 0..count {
                if !wait_irq(channel) {
                    ide_unlock();
                    return print_error(drive, 3);
                }

                let status = ata_read(channel, ATA_REG_STATUS);
                if status & ATA_SR_ERR != 0 { ide_unlock(); return print_error(drive, 2); }
                if status & ATA_SR_DF  != 0 { ide_unlock(); return print_error(drive, 1); }
                if status & ATA_SR_DRQ == 0 { ide_unlock(); return print_error(drive, 3); }

                let offset = (done + s) * SECTOR_SIZE;
                let mut tmp = [0u8; SECTOR_SIZE];
                read_sector(channel, &mut tmp);
                buffer[offset..offset + SECTOR_SIZE].copy_from_slice(&tmp);

                clear_irq(channel);
            }

            done += count;
        }
        0
    };
    ide_unlock();
    result
}

/// Writes `data` starting at `lba` to the given drive using polling.
pub fn ide_write_sectors(drive: usize, lba: u64, data: &[u8]) -> u8 {
    ide_lock();
    let result = unsafe {
        let dev = &IDE_DEVICES[drive];
        if dev.reserved == 0 { ide_unlock(); return 1; }

        let channel   = dev.channel;
        let drive_bit = dev.drive;
        let lba48     = dev.lba48 || lba >= 0x10000000;
        let total     = (data.len() + SECTOR_SIZE - 1) / SECTOR_SIZE;
        let mut done  = 0usize;

        while done < total {
            let count = core::cmp::min(MAX_SECTORS_PER_TRANSFER, total - done);

            // disable IRQs for the polling write path (nIEN=1)
            ata_write_ctrl(channel, 0x02);

            issue_command(channel, drive_bit, lba + done as u64, count, lba48, true);

            for s in 0..count {
                // drive asserts DRQ when ready to accept sector data
                if !poll_drq(channel) {
                    ata_write_ctrl(channel, 0x00);
                    ide_unlock();
                    return print_error(drive, 3);
                }

                let offset = (done + s) * SECTOR_SIZE;
                let bytes_left = data.len().saturating_sub(offset);
                let mut sector_buf = [0u8; SECTOR_SIZE];
                let copy = core::cmp::min(SECTOR_SIZE, bytes_left);
                sector_buf[..copy].copy_from_slice(&data[offset..offset + copy]);
                write_sector(channel, &sector_buf);

                // wait for BSY to clear before checking status or writing next sector
                if !poll_bsy(channel) {
                    ata_write_ctrl(channel, 0x00);
                    ide_unlock();
                    return print_error(drive, 3);
                }

                let status = ata_read(channel, ATA_REG_STATUS);
                if status & ATA_SR_ERR != 0 {
                    ata_write_ctrl(channel, 0x00);
                    ide_unlock();
                    return print_error(drive, 2);
                }
                if status & ATA_SR_DF != 0 {
                    ata_write_ctrl(channel, 0x00);
                    ide_unlock();
                    return print_error(drive, 1);
                }
            }

            // flush write cache to disk
            let flush = if lba48 { ATA_CMD_CACHE_FLUSH_EXT } else { ATA_CMD_CACHE_FLUSH };
            ata_write(channel, ATA_REG_COMMAND, flush);
            poll_bsy(channel);

            // re-enable IRQs
            ata_write_ctrl(channel, 0x00);
            done += count;
        }
        0
    };
    ide_unlock();
    result
}

fn detect_device(channel: usize, drive: usize) -> bool {
    let idx = channel * 2 + drive;
    unsafe {
        IDE_DEVICES[idx] = IdeDevice::zeroed();
        IDE_DEVICES[idx].channel = channel;
        IDE_DEVICES[idx].drive = drive as u8;

        ata_write(channel, ATA_REG_HDDEVSEL, 0xA0 | ((drive as u8) << 4));
        delay400ns(channel);
        ata_write(channel, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
        delay400ns(channel);

        // status 0x00 means no drive present
        if ata_read(channel, ATA_REG_STATUS) == 0 { return false; }

        let mut t = 100_000u32;
        loop {
            let s = ata_read(channel, ATA_REG_STATUS);
            if s & ATA_SR_ERR != 0 { return false; }
            if s & ATA_SR_BSY == 0 && s & ATA_SR_DRQ != 0 { break; }
            t -= 1;
            if t == 0 { return false; }
        }

        let mut buf = [0u8; SECTOR_SIZE];
        read_sector(channel, &mut buf);

        // ATA model strings are byte-swapped
        let mut model = [0u8; 41];
        for i in 0..20 {
            model[i * 2]     = buf[ATA_IDENT_MODEL + i * 2 + 1];
            model[i * 2 + 1] = buf[ATA_IDENT_MODEL + i * 2];
        }
        let mut end = 40;
        while end > 0 && model[end - 1] == b' ' { end -= 1; }
        model[end] = 0;

        IDE_DEVICES[idx].model    = model;
        IDE_DEVICES[idx].reserved = 1;

        let cmdsets = u16::from_le_bytes([
            buf[ATA_IDENT_COMMANDSETS], buf[ATA_IDENT_COMMANDSETS + 1]
        ]);
        let lba48 = cmdsets & (1 << 10) != 0;
        IDE_DEVICES[idx].lba48 = lba48;

        if lba48 {
            IDE_DEVICES[idx].size = u64::from_le_bytes([
                buf[ATA_IDENT_MAX_LBA_EXT],     buf[ATA_IDENT_MAX_LBA_EXT + 1],
                buf[ATA_IDENT_MAX_LBA_EXT + 2], buf[ATA_IDENT_MAX_LBA_EXT + 3],
                buf[ATA_IDENT_MAX_LBA_EXT + 4], buf[ATA_IDENT_MAX_LBA_EXT + 5],
                0, 0,
            ]);
        } else {
            IDE_DEVICES[idx].size = u64::from_le_bytes([
                buf[ATA_IDENT_MAX_LBA],     buf[ATA_IDENT_MAX_LBA + 1],
                buf[ATA_IDENT_MAX_LBA + 2], buf[ATA_IDENT_MAX_LBA + 3],
                0, 0, 0, 0,
            ]);
        }

        println!("Device {}: {} {} sectors, model: {}",
            idx,
            if lba48 { "LBA48," } else { "LBA28," },
            IDE_DEVICES[idx].size,
            core::str::from_utf8(&model[..end]).unwrap_or("?"));

        COUNT += 1;
        true
    }
}

/// Initialises both IDE channels and detects all attached drives.
pub fn ide_init(bar0: u16, bar1: u16, bar2: u16, bar3: u16, bar4: u16) {
    unsafe {
        // use PCI BARs if provided, otherwise fall back to legacy ISA ports
        CHANNELS[ATA_PRIMARY].base  = if bar0 > 1 { bar0 & 0xFFFC } else { ATA_PRIMARY_BASE };
        CHANNELS[ATA_PRIMARY].ctrl  = if bar1 > 1 { bar1 & 0xFFFC } else { ATA_PRIMARY_CTRL };
        CHANNELS[ATA_PRIMARY].bmide = if bar4 > 0 { bar4 & 0xFFFC } else { 0 };

        CHANNELS[ATA_SECONDARY].base  = if bar2 > 1 { bar2 & 0xFFFC } else { ATA_SECONDARY_BASE };
        CHANNELS[ATA_SECONDARY].ctrl  = if bar3 > 1 { bar3 & 0xFFFC } else { ATA_SECONDARY_CTRL };
        CHANNELS[ATA_SECONDARY].bmide = if bar4 > 0 { (bar4 & 0xFFFC) + 8 } else { 0 };

        // enable IRQs on both channels (nIEN=0)
        ata_write_ctrl(ATA_PRIMARY, 0x00);
        ata_write_ctrl(ATA_SECONDARY, 0x00);
    }

    for ch in 0..2 {
        for dr in 0..2 {
            detect_device(ch, dr);
        }
    }

    unsafe {
        let count = COUNT;
        if count == 0 {
            println!("IDE: no devices found");
        } else {
            println!("IDE: {} device(s) detected", count);
        }
    }
}