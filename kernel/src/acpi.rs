use limine::request::RsdpRequest;

unsafe extern "C" {
    static RSDP_REQUEST: RsdpRequest;
}

#[repr(C, packed)]
struct RSDP {
    signature: [u8; 8],
    checksum: u8,
    oem_id: [u8; 6],
    revision: u8,
    rsdt_ptr: u32,
}

impl RSDP {
    fn new() -> &'static Self {
        unsafe {
           let rsdp_res = RSDP_REQUEST.get_response().unwrap();
            &*(rsdp_res.address() as *const RSDP)
        }
    }
}

#[repr(C, packed)]
struct ACPISDTHeader {
    signature: [u8; 4],
    length: u32,
    revision: u8,
    checksum: u8,
    oem_id: [u8; 6],
    oem_table_id: [u8; 8],
    oem_revision: u32,
    creator_id: u32,
    creator_revision: u32,
}

#[repr(C, packed)]
struct XSDT {
    header: ACPISDTHeader,
}

impl XSDT {
    fn entries(&self) -> &'static [u64] {
        let count =
            (self.header.length as usize - core::mem::size_of::<ACPISDTHeader>()) / 8;
        let base = unsafe { (self as *const _ as *const u8)
            .add(core::mem::size_of::<ACPISDTHeader>()) } as *const u64;
        unsafe { core::slice::from_raw_parts(base, count) }
    }
}
