#[repr(C, packed)]
pub struct MPFloatingPoint {
    pub signature: [u8; 4],
    pub mp_configuration_table: u32,
    pub length: u8,
    pub version: u8,
    pub checksum: u8,
    pub feature_bytes: [u8; 5],
}