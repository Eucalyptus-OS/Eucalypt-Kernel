#![no_std]
#![feature(abi_x86_interrupt)]

extern crate alloc;

// Modules
pub mod idt;
pub mod elf;
pub mod mp;
pub mod keyboard;

// C functions go here
unsafe extern "C" {
    pub unsafe fn jump_usermode(entry: u64) -> !;
}