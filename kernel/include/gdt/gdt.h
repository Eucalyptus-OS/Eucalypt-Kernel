#pragma once

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint32_t reserved0;        // 0x00
    uint64_t rsp0;             // 0x04 - 0x08
    uint64_t rsp1;             // 0x0C - 0x10
    uint64_t rsp2;             // 0x14 - 0x18
    uint64_t reserved1;        // 0x1C - 0x20
    uint64_t ist1;             // 0x24 - 0x28
    uint64_t ist2;             // 0x2C - 0x30
    uint64_t ist3;             // 0x34 - 0x38
    uint64_t ist4;             // 0x3C - 0x40
    uint64_t ist5;             // 0x44 - 0x48
    uint64_t ist6;             // 0x4C - 0x50
    uint64_t ist7;             // 0x54 - 0x58
    uint64_t reserved2;        // 0x5C - 0x60
    uint16_t reserved3;        // 0x64
    uint16_t iopb_offset;      // 0x64
} tss_t;

extern tss_t tss;

void gdt_init();