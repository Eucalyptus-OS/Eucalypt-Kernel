use core::{
    alloc::{GlobalAlloc, Layout}, mem, ptr::null_mut,
    sync::atomic::{AtomicPtr, AtomicUsize, Ordering},
};
use spin::Mutex;
use limine::{request::MemmapResponse, memmap::MEMMAP_USABLE};

static HEAP_START:  AtomicPtr<u8> = AtomicPtr::new(null_mut());
static HEAP_SIZE:   AtomicUsize   = AtomicUsize::new(0);
static HEAP_OFFSET: AtomicUsize   = AtomicUsize::new(0);

struct LinkedListBlock {
    size: usize,
    next: *mut LinkedListBlock,
    prev: *mut LinkedListBlock,
}

struct LinkedList {
    head:  *mut LinkedListBlock,
    tail:  *mut LinkedListBlock,
    count: usize,
}

// raw pointers are managed manually, safety is upheld by the allocator
unsafe impl Send for LinkedList {}

impl LinkedList {
    const fn new() -> Self {
        LinkedList { head: null_mut(), tail: null_mut(), count: 0 }
    }

    // appends a block to the tail of the list
    unsafe fn push_back(&mut self, block: *mut LinkedListBlock) {
        unsafe {
            (*block).next = null_mut();
            (*block).prev = self.tail;

            if !self.tail.is_null() {
                (*self.tail).next = block;
            } else {
                self.head = block;
            }

            self.tail = block;
            self.count += 1;
        }
    }

    // removes and returns the block at the head of the list
    unsafe fn pop_front(&mut self) -> *mut LinkedListBlock {
        unsafe {
            if self.head.is_null() {
                return null_mut();
            }

            let front = self.head;
            self.head = (*front).next;

            if !self.head.is_null() {
                (*self.head).prev = null_mut();
            } else {
                self.tail = null_mut();
            }

            self.count -= 1;
            front
        }
    }

    // unlinks an arbitrary block from the list
    unsafe fn remove(&mut self, block: *mut LinkedListBlock) {
        unsafe {
            if (*block).prev.is_null() {
                self.head = (*block).next;
            } else {
                (*(*block).prev).next = (*block).next;
            }

            if (*block).next.is_null() {
                self.tail = (*block).prev;
            } else {
                (*(*block).next).prev = (*block).prev;
            }

            self.count -= 1;
        }
    }

    fn is_empty(&self) -> bool {
        self.head.is_null()
    }
}

static FREE_LIST: Mutex<LinkedList> = Mutex::new(LinkedList {
    head: null_mut(), tail: null_mut(), count: 0,
});

pub struct LinkAllocator;

unsafe impl GlobalAlloc for LinkAllocator {
    // searches the free list first, then bumps the heap pointer
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe {
            let mut free_list = FREE_LIST.lock();
            let mut current = free_list.head;

            while !current.is_null() {
                if (*current).size >= layout.size() {
                    free_list.remove(current);
                    return (current as *mut u8).add(mem::size_of::<LinkedListBlock>());
                }
                current = (*current).next;
            }

            let heap_start = HEAP_START.load(Ordering::Relaxed);
            if heap_start.is_null() {
                return null_mut();
            }

            let align          = layout.align().max(mem::align_of::<LinkedListBlock>());
            let aligned_offset = (HEAP_OFFSET.load(Ordering::Relaxed) + align - 1) & !(align - 1);
            let total_size     = mem::size_of::<LinkedListBlock>() + layout.size();

            if aligned_offset + total_size > HEAP_SIZE.load(Ordering::Relaxed) {
                return null_mut();
            }

            let block = heap_start.add(aligned_offset) as *mut LinkedListBlock;
            (*block).size = layout.size();
            (*block).next = null_mut();
            (*block).prev = null_mut();

            HEAP_OFFSET.store(aligned_offset + total_size, Ordering::Relaxed);

            (block as *mut u8).add(mem::size_of::<LinkedListBlock>())
        }
    }

    // returns the block to the free list for later reuse
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        unsafe {
            if ptr.is_null() {
                return;
            }

            let block = ptr.sub(mem::size_of::<LinkedListBlock>()) as *mut LinkedListBlock;
            FREE_LIST.lock().push_back(block);
        }
    }
}

// grows the heap by increment bytes, returns the old break pointer
pub fn sbrk(increment: isize) -> *mut u8 {
    let heap_start = HEAP_START.load(Ordering::Relaxed);
    if heap_start.is_null() {
        return null_mut();
    }

    let current    = HEAP_OFFSET.load(Ordering::Relaxed) as isize;
    let new_offset = current + increment;

    if new_offset < 0 || new_offset as usize > HEAP_SIZE.load(Ordering::Relaxed) {
        return null_mut();
    }

    HEAP_OFFSET.store(new_offset as usize, Ordering::Relaxed);
    unsafe { heap_start.add(current as usize) }
}

// returns the current heap break address without modifying it
pub fn brk_current() -> *mut u8 {
    let heap_start = HEAP_START.load(Ordering::Relaxed);
    if heap_start.is_null() {
        return null_mut();
    }
    unsafe { heap_start.add(HEAP_OFFSET.load(Ordering::Relaxed)) }
}

#[global_allocator]
static ALLOCATOR: LinkAllocator = LinkAllocator;

// initialises the heap using the first suitably large usable memory region
pub fn init_allocator(memory_map: &MemmapResponse) {
    *FREE_LIST.lock() = LinkedList::new();

    let bitmap_size_bytes = {
        let mut max_addr = 0u64;
        for entry in memory_map.entries() {
            let end = entry.base + entry.length;
            if end > max_addr { max_addr = end; }
        }
        let total_frames = (max_addr as usize + 4095) / 4096;
        let bitmap_size  = (total_frames + 63) / 64;
        (bitmap_size * 8 + 4095) & !4095
    };

    for entry in memory_map.entries() {
        if entry.type_ == MEMMAP_USABLE && entry.length > 16 * 1024 * 1024 {
            let heap_phys = entry.base + bitmap_size_bytes as u64;
            let heap_len  = entry.length as usize - bitmap_size_bytes;

            HEAP_START.store((heap_phys + 0xFFFF800000000000) as *mut u8, Ordering::Relaxed);
            HEAP_SIZE.store(heap_len, Ordering::Relaxed);
            HEAP_OFFSET.store(0, Ordering::Relaxed);
            return;
        }
    }

    panic!("No usable memory found for heap");
}