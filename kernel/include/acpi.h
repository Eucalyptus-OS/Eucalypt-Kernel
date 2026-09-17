#pragma once

#include <stdint.h>

extern uint64_t lapic_addr;
extern uint64_t ioapic_addr;

void acpi_parse_tables();
