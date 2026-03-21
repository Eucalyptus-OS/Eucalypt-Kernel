#![no_std]

extern crate alloc;
use alloc::boxed::Box;

use limine::response::ModuleResponse;
use framebuffer::println;
use vfs::{vfs_mount, RamFs};

pub fn init_ramdisk(module_response: &ModuleResponse) -> (*mut u8, u64) {
    if module_response.modules().iter().count() < 1 {
        panic!("Modules ramfs not found");
    }

    let module = module_response.modules()[0];
    let module_addr = module.addr();
    let module_size = module.size();
    
    // Check if it's at least a floppy image size
    if module_size < 1474560 {
        panic!("Wrong module size: expected >= 1474560, got {}", module_size);
    }
    
    println!("Ramdisk Address: {:?}, Size: {}", module_addr, module_size);
    (module_addr, module_size)
}

/// Initialises the ramdisk from Limine and mounts it into the VFS.
pub fn mount_ramdisk(module_response: &ModuleResponse, mount_point: &'static str) -> Result<(), &'static str> {
    let (addr, size) = init_ramdisk(module_response);
    
    let ramfs = RamFs::new();
    ramfs.load_from_fat12(addr, size)?;
    
    vfs_mount(mount_point, Box::new(ramfs))?;
    println!("Ramdisk mounted at /{}", mount_point);
    
    Ok(())
}