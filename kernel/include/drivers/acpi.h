#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ISA_IRQ_COUNT 16

typedef struct {
	uint8_t gsi;
	bool active_low;
	bool level_triggered;
	bool present;
} isa_irq_override_t;

extern uint64_t lapic_addr;
extern uint64_t ioapic_addr;
extern isa_irq_override_t isa_irq_overrides[ISA_IRQ_COUNT];

void acpi_parse_tables();
uint8_t acpi_isa_irq_gsi(uint8_t isa_irq);
bool acpi_isa_irq_active_low(uint8_t isa_irq);
bool acpi_isa_irq_level_triggered(uint8_t isa_irq);