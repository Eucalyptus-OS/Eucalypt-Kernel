#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct PhysAddr(u64);

#[repr(transparent)]
#[derive(Clone, Copy)]
pub struct VirtAddr(u64);

impl PhysAddr {
    pub fn new(addr: u64) -> Self {
        PhysAddr(addr)
    }
    
    pub fn as_u64(&self) -> u64 {
        self.0
    }
    
    pub fn align_up(&self, align: u64) -> Self {
        PhysAddr((self.0 + align - 1) & !(align - 1))
    }
    
    pub fn align_down(&self, align: u64) -> Self {
        PhysAddr(self.0 & !(align - 1))
    }
}

impl VirtAddr {
    pub fn new(addr: u64) -> Self {
        VirtAddr(addr)
    }
    
    pub fn as_u64(&self) -> u64 {
        self.0
    }
    
    pub fn page_offset(&self) -> usize {
        (self.0 & 0xFFF) as usize
    }
    
    pub fn p4_index(&self) -> usize {
        ((self.0 >> 39) & 0x1FF) as usize
    }
    
    pub fn p3_index(&self) -> usize {
        ((self.0 >> 30) & 0x1FF) as usize
    }
    
    pub fn p2_index(&self) -> usize {
        ((self.0 >> 21) & 0x1FF) as usize
    }
    
    pub fn p1_index(&self) -> usize {
        ((self.0 >> 12) & 0x1FF) as usize
    }
    
    pub fn align_up(&self, align: u64) -> Self {
        VirtAddr((self.0 + align - 1) & !(align - 1))
    }
    
    pub fn align_down(&self, align: u64) -> Self {
        VirtAddr(self.0 & !(align - 1))
    }
}