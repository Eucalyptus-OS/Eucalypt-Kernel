#![no_std]
#![allow(unused)]

extern crate alloc;

use alloc::vec;
use framebuffer::println;
use ide::{ide_read_sectors, ide_write_sectors, IDE_DEVICES};

mod super_block;
pub use super_block::SuperBlock;

const SECTOR_SIZE: u64 = 512;

pub trait StorageDriver {
    fn read_sector(&self, lba: u64, buffer: &mut [u8]) -> bool;
    fn write_sector(&self, lba: u64, data: &[u8]) -> bool;
}

pub struct IdeDriver {
    pub drive: usize,
}

impl StorageDriver for IdeDriver {
    fn read_sector(&self, lba: u64, buffer: &mut [u8]) -> bool {
        ide_read_sectors(self.drive, lba, buffer) == 0
    }

    fn write_sector(&self, lba: u64, data: &[u8]) -> bool {
        ide_write_sectors(self.drive, lba, data) == 0
    }
}

fn zero_blocks(drive: usize, start_block: u64, num_blocks: u64, block_size_bytes: u64) -> bool {
    let sectors_per_block = block_size_bytes / SECTOR_SIZE;
    let start_sector = start_block * sectors_per_block;
    let total_sectors = num_blocks * sectors_per_block;
    let zero_sector = [0u8; 512];

    for i in 0..total_sectors {
        let sector_to_write = start_sector + i;
        if ide_write_sectors(drive, sector_to_write, &zero_sector) != 0 {
            println!("Writing zeros failed at sector: {}", sector_to_write);
            return false;
        }
    }

    true
}

fn erase_disk(drive: usize) {
    let sector_count = unsafe { 
        IDE_DEVICES[drive].size 
    };
    let _ = ide_write_sectors(drive, 0, &[]);
}

pub fn write_eucalypt_fs(drive: u8) {
    let drive_usize = drive as usize;
    let super_block = SuperBlock::new(drive);

    println!("SuperBlock Layout: {}", super_block);

    let sb_bytes = super_block.to_bytes();
    if ide_write_sectors(drive_usize, 1, &sb_bytes) != 0 {
        println!("Failed to write superblock");
        return;
    }

    println!("Superblock written to disk.");
}