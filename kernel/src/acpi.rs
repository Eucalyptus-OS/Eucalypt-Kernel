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
    xsdt_ptr: u64,
}

impl RSDP {
    fn get() -> &'static Self {
        unsafe {
            let res = RSDP_REQUEST.get_response().unwrap();
            &*(res.address() as *const RSDP)
        }
    }

    fn xsdt(&self) -> &'static XSDT {
        unsafe { &*(self.xsdt_ptr as *const XSDT) }
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
    fn entries(&self) -> &[u64] {
        let header_size = core::mem::size_of::<ACPISDTHeader>();
        let count = (self.header.length as usize - header_size) / 8;
        let base = unsafe {
            (self as *const _ as *const u8).add(header_size) as *const u64
        };
        unsafe { core::slice::from_raw_parts(base, count) }
    }

    fn find_sdt(&self, sig: &[u8; 4]) -> Option<&'static ACPISDTHeader> {
        for &entry in self.entries() {
            let hdr = unsafe { &*(entry as *const ACPISDTHeader) };
            if &hdr.signature == sig {
                return Some(hdr);
            }
        }
        None
    }
}

pub fn find_acpi_table(sig: &[u8; 4]) -> Option<&'static ACPISDTHeader> {
    let rsdp = RSDP::get();
    let xsdt = rsdp.xsdt();
    xsdt.find_sdt(sig)
}
