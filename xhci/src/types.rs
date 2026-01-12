// Capability Registers
pub const CAP_CAPLENGTH: usize = 0x00;
pub const CAP_HCIVERSION: usize = 0x02;
pub const CAP_HCSPARAMS1: usize = 0x04;
pub const CAP_HCSPARAMS2: usize = 0x08;
pub const CAP_HCSPARAMS3: usize = 0x0C;
pub const CAP_HCCPARAMS1: usize = 0x10;
pub const CAP_DBOFF: usize = 0x14;
pub const CAP_RTSOFF: usize = 0x18;
pub const CAP_HCCPARAMS2: usize = 0x1C;

// HCSPARAMS1 Fields
pub const HCSPARAMS1_MAX_SLOTS_MASK: u32 = 0xFF;
pub const HCSPARAMS1_MAX_SLOTS_SHIFT: u32 = 0;
pub const HCSPARAMS1_MAX_INTRS_MASK: u32 = 0x7FF;
pub const HCSPARAMS1_MAX_INTRS_SHIFT: u32 = 8;
pub const HCSPARAMS1_MAX_PORTS_MASK: u32 = 0xFF;
pub const HCSPARAMS1_MAX_PORTS_SHIFT: u32 = 24;

// HCSPARAMS2 Fields
pub const HCSPARAMS2_IST_MASK: u32 = 0xF;
pub const HCSPARAMS2_IST_SHIFT: u32 = 0;
pub const HCSPARAMS2_ERST_MAX_MASK: u32 = 0xF;
pub const HCSPARAMS2_ERST_MAX_SHIFT: u32 = 4;
pub const HCSPARAMS2_MAX_SCRATCHPAD_BUFS_HI_MASK: u32 = 0x1F;
pub const HCSPARAMS2_MAX_SCRATCHPAD_BUFS_HI_SHIFT: u32 = 21;
pub const HCSPARAMS2_MAX_SCRATCHPAD_BUFS_LO_MASK: u32 = 0x1F;
pub const HCSPARAMS2_MAX_SCRATCHPAD_BUFS_LO_SHIFT: u32 = 27;

// HCSPARAMS3 Fields
pub const HCSPARAMS3_U1_DEVICE_EXIT_LAT_MASK: u32 = 0xFF;
pub const HCSPARAMS3_U1_DEVICE_EXIT_LAT_SHIFT: u32 = 0;
pub const HCSPARAMS3_U2_DEVICE_EXIT_LAT_MASK: u32 = 0xFFFF;
pub const HCSPARAMS3_U2_DEVICE_EXIT_LAT_SHIFT: u32 = 16;

// HCCPARAMS1 Fields
pub const HCCPARAMS1_AC64: u32 = 1 << 0;
pub const HCCPARAMS1_BNC: u32 = 1 << 1;
pub const HCCPARAMS1_CSZ: u32 = 1 << 2;
pub const HCCPARAMS1_PPC: u32 = 1 << 3;
pub const HCCPARAMS1_PIND: u32 = 1 << 4;
pub const HCCPARAMS1_LHRC: u32 = 1 << 5;
pub const HCCPARAMS1_LTC: u32 = 1 << 6;
pub const HCCPARAMS1_NSS: u32 = 1 << 7;
pub const HCCPARAMS1_PAE: u32 = 1 << 8;
pub const HCCPARAMS1_SPC: u32 = 1 << 9;
pub const HCCPARAMS1_SEC: u32 = 1 << 10;
pub const HCCPARAMS1_CFC: u32 = 1 << 11;
pub const HCCPARAMS1_MAX_PSA_SIZE_MASK: u32 = 0xF;
pub const HCCPARAMS1_MAX_PSA_SIZE_SHIFT: u32 = 12;
pub const HCCPARAMS1_XECP_MASK: u32 = 0xFFFF;
pub const HCCPARAMS1_XECP_SHIFT: u32 = 16;

// HCCPARAMS2 Fields
pub const HCCPARAMS2_U3C: u32 = 1 << 0;
pub const HCCPARAMS2_CMC: u32 = 1 << 1;
pub const HCCPARAMS2_FSC: u32 = 1 << 2;
pub const HCCPARAMS2_CTC: u32 = 1 << 3;
pub const HCCPARAMS2_LEC: u32 = 1 << 4;
pub const HCCPARAMS2_CIC: u32 = 1 << 5;
pub const HCCPARAMS2_ETC: u32 = 1 << 6;
pub const HCCPARAMS2_ETC_TSC: u32 = 1 << 7;
pub const HCCPARAMS2_GSC: u32 = 1 << 8;
pub const HCCPARAMS2_VTC: u32 = 1 << 9;

// Operational Registers
pub const OP_USBCMD: usize = 0x00;
pub const OP_USBSTS: usize = 0x04;
pub const OP_PAGESIZE: usize = 0x08;
pub const OP_DNCTRL: usize = 0x14;
pub const OP_CRCR: usize = 0x18;
pub const OP_DCBAAP: usize = 0x30;
pub const OP_CONFIG: usize = 0x38;

// USBCMD Bits
pub const USBCMD_RS: u32 = 1 << 0;
pub const USBCMD_HCRST: u32 = 1 << 1;
pub const USBCMD_INTE: u32 = 1 << 2;
pub const USBCMD_HSEE: u32 = 1 << 3;
pub const USBCMD_LHCRST: u32 = 1 << 7;
pub const USBCMD_CSS: u32 = 1 << 8;
pub const USBCMD_CRS: u32 = 1 << 9;
pub const USBCMD_EWE: u32 = 1 << 10;
pub const USBCMD_EU3S: u32 = 1 << 11;
pub const USBCMD_CME: u32 = 1 << 13;
pub const USBCMD_ETE: u32 = 1 << 14;
pub const USBCMD_TSC_EN: u32 = 1 << 15;
pub const USBCMD_VTIOE: u32 = 1 << 16;

// USBSTS Bits
pub const USBSTS_HCH: u32 = 1 << 0;
pub const USBSTS_HSE: u32 = 1 << 2;
pub const USBSTS_EINT: u32 = 1 << 3;
pub const USBSTS_PCD: u32 = 1 << 4;
pub const USBSTS_SSS: u32 = 1 << 8;
pub const USBSTS_RSS: u32 = 1 << 9;
pub const USBSTS_SRE: u32 = 1 << 10;
pub const USBSTS_CNR: u32 = 1 << 11;
pub const USBSTS_HCE: u32 = 1 << 12;

// CRCR Bits
pub const CRCR_RCS: u64 = 1 << 0;
pub const CRCR_CS: u64 = 1 << 1;
pub const CRCR_CA: u64 = 1 << 2;
pub const CRCR_CRR: u64 = 1 << 3;
pub const CRCR_PTR_MASK: u64 = !0x3F;

// CONFIG Register
pub const CONFIG_MAX_SLOTS_EN_MASK: u32 = 0xFF;
pub const CONFIG_MAX_SLOTS_EN_SHIFT: u32 = 0;
pub const CONFIG_U3E: u32 = 1 << 8;
pub const CONFIG_CIE: u32 = 1 << 9;

// Port Register Set (per port, stride 0x10)
pub const PORT_SC: usize = 0x00;
pub const PORT_PMSC: usize = 0x04;
pub const PORT_LI: usize = 0x08;
pub const PORT_HLPMC: usize = 0x0C;

// PORTSC Bits
pub const PORTSC_CCS: u32 = 1 << 0;
pub const PORTSC_PED: u32 = 1 << 1;
pub const PORTSC_OCA: u32 = 1 << 3;
pub const PORTSC_PR: u32 = 1 << 4;
pub const PORTSC_PLS_MASK: u32 = 0xF;
pub const PORTSC_PLS_SHIFT: u32 = 5;
pub const PORTSC_PP: u32 = 1 << 9;
pub const PORTSC_PORT_SPEED_MASK: u32 = 0xF;
pub const PORTSC_PORT_SPEED_SHIFT: u32 = 10;
pub const PORTSC_PIC_MASK: u32 = 0x3;
pub const PORTSC_PIC_SHIFT: u32 = 14;
pub const PORTSC_LWS: u32 = 1 << 16;
pub const PORTSC_CSC: u32 = 1 << 17;
pub const PORTSC_PEC: u32 = 1 << 18;
pub const PORTSC_WRC: u32 = 1 << 19;
pub const PORTSC_OCC: u32 = 1 << 20;
pub const PORTSC_PRC: u32 = 1 << 21;
pub const PORTSC_PLC: u32 = 1 << 22;
pub const PORTSC_CEC: u32 = 1 << 23;
pub const PORTSC_CAS: u32 = 1 << 24;
pub const PORTSC_WCE: u32 = 1 << 25;
pub const PORTSC_WDE: u32 = 1 << 26;
pub const PORTSC_WOE: u32 = 1 << 27;
pub const PORTSC_DR: u32 = 1 << 30;
pub const PORTSC_WPR: u32 = 1 << 31;

// Port Link State Values
pub const PLS_U0: u32 = 0;
pub const PLS_U1: u32 = 1;
pub const PLS_U2: u32 = 2;
pub const PLS_U3: u32 = 3;
pub const PLS_DISABLED: u32 = 4;
pub const PLS_RX_DETECT: u32 = 5;
pub const PLS_INACTIVE: u32 = 6;
pub const PLS_POLLING: u32 = 7;
pub const PLS_RECOVERY: u32 = 8;
pub const PLS_HOT_RESET: u32 = 9;
pub const PLS_COMPLIANCE_MODE: u32 = 10;
pub const PLS_TEST_MODE: u32 = 11;
pub const PLS_RESUME: u32 = 15;

// PORTPMSC Bits
pub const PORTPMSC_U1_TIMEOUT_MASK: u32 = 0xFF;
pub const PORTPMSC_U1_TIMEOUT_SHIFT: u32 = 0;
pub const PORTPMSC_U2_TIMEOUT_MASK: u32 = 0xFF;
pub const PORTPMSC_U2_TIMEOUT_SHIFT: u32 = 8;
pub const PORTPMSC_FLA: u32 = 1 << 16;

// Runtime Registers
pub const RT_MFINDEX: usize = 0x00;
pub const RT_IR0_IMAN: usize = 0x20;
pub const RT_IR0_IMOD: usize = 0x24;
pub const RT_IR0_ERSTSZ: usize = 0x28;
pub const RT_IR0_ERSTBA: usize = 0x30;
pub const RT_IR0_ERDP: usize = 0x38;

// Interrupter Register Stride
pub const RT_IR_STRIDE: usize = 0x20;

// IMAN Bits
pub const IMAN_IP: u32 = 1 << 0;
pub const IMAN_IE: u32 = 1 << 1;

// IMOD Fields
pub const IMOD_IMODI_MASK: u32 = 0xFFFF;
pub const IMOD_IMODI_SHIFT: u32 = 0;
pub const IMOD_IMODC_MASK: u32 = 0xFFFF;
pub const IMOD_IMODC_SHIFT: u32 = 16;

// ERSTSZ Fields
pub const ERSTSZ_MASK: u32 = 0xFFFF;

// ERDP Bits
pub const ERDP_DESI_MASK: u64 = 0x7;
pub const ERDP_DESI_SHIFT: u64 = 0;
pub const ERDP_EHB: u64 = 1 << 3;
pub const ERDP_PTR_MASK: u64 = !0xF;

// Doorbell Registers
pub const DB_TARGET_MASK: u32 = 0xFF;
pub const DB_TARGET_SHIFT: u32 = 0;
pub const DB_STREAM_ID_MASK: u32 = 0xFFFF;
pub const DB_STREAM_ID_SHIFT: u32 = 16;

// Doorbell Targets
pub const DB_TARGET_HC_COMMAND: u32 = 0;
pub const DB_TARGET_EP_0_OUT: u32 = 1;
pub const DB_TARGET_EP_0_IN: u32 = 1;

// Extended Capabilities
pub const EXTCAP_ID_MASK: u32 = 0xFF;
pub const EXTCAP_ID_SHIFT: u32 = 0;
pub const EXTCAP_NEXT_MASK: u32 = 0xFF;
pub const EXTCAP_NEXT_SHIFT: u32 = 8;

// Extended Capability IDs
pub const EXTCAP_USB_LEGACY: u32 = 1;
pub const EXTCAP_SUPPORTED_PROTOCOL: u32 = 2;
pub const EXTCAP_EXTENDED_POWER_MANAGEMENT: u32 = 3;
pub const EXTCAP_IO_VIRTUALIZATION: u32 = 4;
pub const EXTCAP_MESSAGE_INTERRUPT: u32 = 5;
pub const EXTCAP_LOCAL_MEMORY: u32 = 6;
pub const EXTCAP_USB_DEBUG: u32 = 10;
pub const EXTCAP_EXTENDED_MESSAGE_INTERRUPT: u32 = 17;

// USB Legacy Support Capability
pub const USBLEGSUP_BIOS_OWNED: u32 = 1 << 16;
pub const USBLEGSUP_OS_OWNED: u32 = 1 << 24;

// TRB Types
pub const TRB_TYPE_NORMAL: u32 = 1;
pub const TRB_TYPE_SETUP_STAGE: u32 = 2;
pub const TRB_TYPE_DATA_STAGE: u32 = 3;
pub const TRB_TYPE_STATUS_STAGE: u32 = 4;
pub const TRB_TYPE_ISOCH: u32 = 5;
pub const TRB_TYPE_LINK: u32 = 6;
pub const TRB_TYPE_EVENT_DATA: u32 = 7;
pub const TRB_TYPE_NOOP: u32 = 8;
pub const TRB_TYPE_ENABLE_SLOT: u32 = 9;
pub const TRB_TYPE_DISABLE_SLOT: u32 = 10;
pub const TRB_TYPE_ADDRESS_DEVICE: u32 = 11;
pub const TRB_TYPE_CONFIGURE_EP: u32 = 12;
pub const TRB_TYPE_EVALUATE_CONTEXT: u32 = 13;
pub const TRB_TYPE_RESET_EP: u32 = 14;
pub const TRB_TYPE_STOP_EP: u32 = 15;
pub const TRB_TYPE_SET_TR_DEQUEUE: u32 = 16;
pub const TRB_TYPE_RESET_DEVICE: u32 = 17;
pub const TRB_TYPE_FORCE_EVENT: u32 = 18;
pub const TRB_TYPE_NEGOTIATE_BW: u32 = 19;
pub const TRB_TYPE_SET_LATENCY_TOLERANCE: u32 = 20;
pub const TRB_TYPE_GET_PORT_BW: u32 = 21;
pub const TRB_TYPE_FORCE_HEADER: u32 = 22;
pub const TRB_TYPE_NOOP_CMD: u32 = 23;
pub const TRB_TYPE_TRANSFER_EVENT: u32 = 32;
pub const TRB_TYPE_COMMAND_COMPLETION: u32 = 33;
pub const TRB_TYPE_PORT_STATUS_CHANGE: u32 = 34;
pub const TRB_TYPE_BANDWIDTH_REQUEST: u32 = 35;
pub const TRB_TYPE_DOORBELL_EVENT: u32 = 36;
pub const TRB_TYPE_HOST_CONTROLLER_EVENT: u32 = 37;
pub const TRB_TYPE_DEVICE_NOTIFICATION: u32 = 38;
pub const TRB_TYPE_MFINDEX_WRAP: u32 = 39;

// TRB Control Field Bits
pub const TRB_CYCLE: u32 = 1 << 0;
pub const TRB_ENT: u32 = 1 << 1;
pub const TRB_ISP: u32 = 1 << 2;
pub const TRB_NS: u32 = 1 << 3;
pub const TRB_CHAIN: u32 = 1 << 4;
pub const TRB_IOC: u32 = 1 << 5;
pub const TRB_IDT: u32 = 1 << 6;
pub const TRB_BEI: u32 = 1 << 9;
pub const TRB_TYPE_MASK: u32 = 0x3F;
pub const TRB_TYPE_SHIFT: u32 = 10;

// TRB Completion Codes
pub const TRB_CC_INVALID: u32 = 0;
pub const TRB_CC_SUCCESS: u32 = 1;
pub const TRB_CC_DATA_BUFFER_ERROR: u32 = 2;
pub const TRB_CC_BABBLE_DETECTED: u32 = 3;
pub const TRB_CC_USB_TRANSACTION_ERROR: u32 = 4;
pub const TRB_CC_TRB_ERROR: u32 = 5;
pub const TRB_CC_STALL_ERROR: u32 = 6;
pub const TRB_CC_RESOURCE_ERROR: u32 = 7;
pub const TRB_CC_BANDWIDTH_ERROR: u32 = 8;
pub const TRB_CC_NO_SLOTS_AVAILABLE: u32 = 9;
pub const TRB_CC_INVALID_STREAM_TYPE: u32 = 10;
pub const TRB_CC_SLOT_NOT_ENABLED: u32 = 11;
pub const TRB_CC_EP_NOT_ENABLED: u32 = 12;
pub const TRB_CC_SHORT_PACKET: u32 = 13;
pub const TRB_CC_RING_UNDERRUN: u32 = 14;
pub const TRB_CC_RING_OVERRUN: u32 = 15;
pub const TRB_CC_VF_EVENT_RING_FULL: u32 = 16;
pub const TRB_CC_PARAMETER_ERROR: u32 = 17;
pub const TRB_CC_BANDWIDTH_OVERRUN: u32 = 18;
pub const TRB_CC_CONTEXT_STATE_ERROR: u32 = 19;
pub const TRB_CC_NO_PING_RESPONSE: u32 = 20;
pub const TRB_CC_EVENT_RING_FULL: u32 = 21;
pub const TRB_CC_INCOMPATIBLE_DEVICE: u32 = 22;
pub const TRB_CC_MISSED_SERVICE: u32 = 23;
pub const TRB_CC_COMMAND_RING_STOPPED: u32 = 24;
pub const TRB_CC_COMMAND_ABORTED: u32 = 25;
pub const TRB_CC_STOPPED: u32 = 26;
pub const TRB_CC_STOPPED_LENGTH_INVALID: u32 = 27;
pub const TRB_CC_STOPPED_SHORT_PACKET: u32 = 28;
pub const TRB_CC_MAX_EXIT_LATENCY_TOO_LARGE: u32 = 29;
pub const TRB_CC_ISOCH_BUFFER_OVERRUN: u32 = 31;
pub const TRB_CC_EVENT_LOST_ERROR: u32 = 32;
pub const TRB_CC_UNDEFINED_ERROR: u32 = 33;
pub const TRB_CC_INVALID_STREAM_ID: u32 = 34;
pub const TRB_CC_SECONDARY_BANDWIDTH_ERROR: u32 = 35;
pub const TRB_CC_SPLIT_TRANSACTION_ERROR: u32 = 36;

// Slot Context Fields (DW0)
pub const SLOT_CTX_ROUTE_STRING_MASK: u32 = 0xFFFFF;
pub const SLOT_CTX_ROUTE_STRING_SHIFT: u32 = 0;
pub const SLOT_CTX_SPEED_MASK: u32 = 0xF;
pub const SLOT_CTX_SPEED_SHIFT: u32 = 20;
pub const SLOT_CTX_MTT: u32 = 1 << 25;
pub const SLOT_CTX_HUB: u32 = 1 << 26;
pub const SLOT_CTX_CONTEXT_ENTRIES_MASK: u32 = 0x1F;
pub const SLOT_CTX_CONTEXT_ENTRIES_SHIFT: u32 = 27;

// Slot Context Fields (DW1)
pub const SLOT_CTX_MAX_EXIT_LATENCY_MASK: u32 = 0xFFFF;
pub const SLOT_CTX_MAX_EXIT_LATENCY_SHIFT: u32 = 0;
pub const SLOT_CTX_ROOT_HUB_PORT_NUM_MASK: u32 = 0xFF;
pub const SLOT_CTX_ROOT_HUB_PORT_NUM_SHIFT: u32 = 16;
pub const SLOT_CTX_NUM_PORTS_MASK: u32 = 0xFF;
pub const SLOT_CTX_NUM_PORTS_SHIFT: u32 = 24;

// Slot Context Fields (DW2)
pub const SLOT_CTX_TT_HUB_SLOT_ID_MASK: u32 = 0xFF;
pub const SLOT_CTX_TT_HUB_SLOT_ID_SHIFT: u32 = 0;
pub const SLOT_CTX_TT_PORT_NUM_MASK: u32 = 0xFF;
pub const SLOT_CTX_TT_PORT_NUM_SHIFT: u32 = 8;
pub const SLOT_CTX_TTT_MASK: u32 = 0x3;
pub const SLOT_CTX_TTT_SHIFT: u32 = 16;
pub const SLOT_CTX_INTERRUPTER_TARGET_MASK: u32 = 0x3FF;
pub const SLOT_CTX_INTERRUPTER_TARGET_SHIFT: u32 = 22;

// Slot Context Fields (DW3)
pub const SLOT_CTX_DEVICE_ADDRESS_MASK: u32 = 0xFF;
pub const SLOT_CTX_DEVICE_ADDRESS_SHIFT: u32 = 0;
pub const SLOT_CTX_SLOT_STATE_MASK: u32 = 0x1F;
pub const SLOT_CTX_SLOT_STATE_SHIFT: u32 = 27;

// Slot States
pub const SLOT_STATE_DISABLED: u32 = 0;
pub const SLOT_STATE_DEFAULT: u32 = 1;
pub const SLOT_STATE_ADDRESSED: u32 = 2;
pub const SLOT_STATE_CONFIGURED: u32 = 3;

// Endpoint Context Fields (DW0)
pub const EP_CTX_EP_STATE_MASK: u32 = 0x7;
pub const EP_CTX_EP_STATE_SHIFT: u32 = 0;
pub const EP_CTX_MULT_MASK: u32 = 0x3;
pub const EP_CTX_MULT_SHIFT: u32 = 8;
pub const EP_CTX_MAX_PSTREAMS_MASK: u32 = 0x1F;
pub const EP_CTX_MAX_PSTREAMS_SHIFT: u32 = 10;
pub const EP_CTX_LSA: u32 = 1 << 15;
pub const EP_CTX_INTERVAL_MASK: u32 = 0xFF;
pub const EP_CTX_INTERVAL_SHIFT: u32 = 16;
pub const EP_CTX_MAX_ESIT_PAYLOAD_HI_MASK: u32 = 0xFF;
pub const EP_CTX_MAX_ESIT_PAYLOAD_HI_SHIFT: u32 = 24;

// Endpoint Context Fields (DW1)
pub const EP_CTX_CERR_MASK: u32 = 0x3;
pub const EP_CTX_CERR_SHIFT: u32 = 1;
pub const EP_CTX_EP_TYPE_MASK: u32 = 0x7;
pub const EP_CTX_EP_TYPE_SHIFT: u32 = 3;
pub const EP_CTX_HID: u32 = 1 << 7;
pub const EP_CTX_MAX_BURST_SIZE_MASK: u32 = 0xFF;
pub const EP_CTX_MAX_BURST_SIZE_SHIFT: u32 = 8;
pub const EP_CTX_MAX_PACKET_SIZE_MASK: u32 = 0xFFFF;
pub const EP_CTX_MAX_PACKET_SIZE_SHIFT: u32 = 16;

// Endpoint Context Fields (DW2)
pub const EP_CTX_DCS: u32 = 1 << 0;
pub const EP_CTX_TR_DEQUEUE_PTR_LO_MASK: u32 = !0xF;

// Endpoint Context Fields (DW4)
pub const EP_CTX_AVG_TRB_LENGTH_MASK: u32 = 0xFFFF;
pub const EP_CTX_AVG_TRB_LENGTH_SHIFT: u32 = 0;
pub const EP_CTX_MAX_ESIT_PAYLOAD_LO_MASK: u32 = 0xFFFF;
pub const EP_CTX_MAX_ESIT_PAYLOAD_LO_SHIFT: u32 = 16;

// Endpoint States
pub const EP_STATE_DISABLED: u32 = 0;
pub const EP_STATE_RUNNING: u32 = 1;
pub const EP_STATE_HALTED: u32 = 2;
pub const EP_STATE_STOPPED: u32 = 3;
pub const EP_STATE_ERROR: u32 = 4;

// Endpoint Types
pub const EP_TYPE_ISOCH_OUT: u32 = 1;
pub const EP_TYPE_BULK_OUT: u32 = 2;
pub const EP_TYPE_INTERRUPT_OUT: u32 = 3;
pub const EP_TYPE_CONTROL: u32 = 4;
pub const EP_TYPE_ISOCH_IN: u32 = 5;
pub const EP_TYPE_BULK_IN: u32 = 6;
pub const EP_TYPE_INTERRUPT_IN: u32 = 7;

// Device Speeds
pub const SPEED_FULL: u32 = 1;
pub const SPEED_LOW: u32 = 2;
pub const SPEED_HIGH: u32 = 3;
pub const SPEED_SUPER: u32 = 4;
pub const SPEED_SUPER_PLUS: u32 = 5;

// Context Sizes
pub const CONTEXT_SIZE_32: usize = 32;
pub const CONTEXT_SIZE_64: usize = 64;

// Alignment Requirements
pub const ALIGNMENT_TRB: usize = 16;
pub const ALIGNMENT_SEGMENT: usize = 64;
pub const ALIGNMENT_PAGE: usize = 4096;
pub const ALIGNMENT_DCBAA: usize = 64;
pub const ALIGNMENT_ERST: usize = 64;
pub const ALIGNMENT_SCRATCHPAD: usize = 4096;