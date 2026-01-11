//! Physical frame allocator using a bitmap

use core::ptr::null_mut;
use limine::{memory_map::EntryType, response::MemoryMapResponse};
use super::addr::PhysAddr;

const PAGE_SIZE: usize = 4096;

static mut FRAME_BITMAP: *mut u64 = null_mut();
static mut TOTAL_FRAMES: usize = 0;
static mut BITMAP_SIZE: usize = 0;

pub struct FrameAllocator;

impl FrameAllocator {
    pub unsafe fn init(memory_map: &MemoryMapResponse) {
        let mut max_addr = 0u64;
        
        for entry in memory_map.entries() {
            let end = entry.base + entry.length;
            if end > max_addr {
                max_addr = end;
            }
        }
        
        TOTAL_FRAMES = (max_addr / PAGE_SIZE as u64) as usize;
        BITMAP_SIZE = (TOTAL_FRAMES + 63) / 64;
        
        for entry in memory_map.entries() {
            if entry.entry_type == EntryType::USABLE && entry.length >= (BITMAP_SIZE * 8) as u64 {
                FRAME_BITMAP = (entry.base + 0xFFFF800000000000) as *mut u64;
                
                for i in 0..BITMAP_SIZE {
                    *FRAME_BITMAP.add(i) = 0xFFFFFFFFFFFFFFFF;
                }
                
                let bitmap_frames = (BITMAP_SIZE * 8 + PAGE_SIZE - 1) / PAGE_SIZE;
                for i in 0..bitmap_frames {
                    let frame = (entry.base as usize / PAGE_SIZE) + i;
                    Self::mark_used(frame);
                }
                
                break;
            }
        }
        
        for entry in memory_map.entries() {
            if entry.entry_type == EntryType::USABLE {
                let start_frame = (entry.base as usize) / PAGE_SIZE;
                let frame_count = (entry.length as usize) / PAGE_SIZE;
                
                for i in 0..frame_count {
                    Self::mark_free(start_frame + i);
                }
            }
        }
    }
    
    unsafe fn mark_free(frame: usize) {
        if frame >= TOTAL_FRAMES {
            return;
        }
        let index = frame / 64;
        let bit = frame % 64;
        *FRAME_BITMAP.add(index) |= 1u64 << bit;
    }
    
    unsafe fn mark_used(frame: usize) {
        if frame >= TOTAL_FRAMES {
            return;
        }
        let index = frame / 64;
        let bit = frame % 64;
        *FRAME_BITMAP.add(index) &= !(1u64 << bit);
    }
    
    pub unsafe fn alloc_frame() -> Option<PhysAddr> {
        for i in 0..BITMAP_SIZE {
            let bitmap = *FRAME_BITMAP.add(i);
            if bitmap != 0 {
                let bit = bitmap.trailing_zeros() as usize;
                let frame = i * 64 + bit;
                
                if frame < TOTAL_FRAMES {
                    Self::mark_used(frame);
                    return Some(PhysAddr::new((frame * PAGE_SIZE) as u64));
                }
            }
        }
        None
    }
    
    pub unsafe fn free_frame(addr: PhysAddr) {
        let frame = (addr.as_u64() / PAGE_SIZE as u64) as usize;
        Self::mark_free(frame);
    }
}