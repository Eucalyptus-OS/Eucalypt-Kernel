#![no_std]
#![feature(abi_x86_interrupt)]
#![feature(alloc_error_handler)]

extern crate alloc;

// Modules
pub mod gdt;
pub mod idt;
pub mod elf;
pub mod smp;

// Re-exports
pub use memory::allocator::init_allocator;
pub use memory::addr::{PhysAddr, VirtAddr};
pub use memory::vmm::{VMM, PageTableEntry};

// C functions go here
unsafe extern "C" {
}