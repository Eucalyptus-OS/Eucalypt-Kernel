#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <limine.h>

extern volatile struct limine_mp_request mp_request;

typedef struct cpu {
    uint8_t cpu_id;
    uint8_t lapic_id;
    uint64_t reserved;
} cpu_t;

extern uint8_t cpu_count;
extern uint8_t bsp_lapic_id;
extern cpu_t cpus[100];

bool smp_is_cpu_online(uint8_t pid);
uint8_t smp_online_count(void);
uint8_t smp_get_apic_id(uint8_t pid);
uint8_t smp_init();
