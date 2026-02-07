#![no_std]
#![feature(abi_x86_interrupt)]
#![feature(alloc_error_handler)]

extern crate alloc;

// Modules
pub mod gdt;
pub mod idt;
pub mod elf;
pub mod mp;

// C functions go here
unsafe extern "C" {
}