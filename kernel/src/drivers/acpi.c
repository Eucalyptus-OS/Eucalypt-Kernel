#include <stddef.h>
#include <limine.h>
#include <mm/frame.h>
#include <mem.h>
#include <mm/hhdm.h>
#include <logging/printk.h>
#include <drivers/acpi.h>

uint64_t lapic_addr = 0;
uint64_t ioapic_addr = 0;
isa_irq_override_t isa_irq_overrides[ISA_IRQ_COUNT];

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
	.id = LIMINE_RSDP_REQUEST_ID,
	.revision = 0
};

struct rsdp {
	char signature[8];
	uint8_t checksum;
	char oem_id[6];
	uint8_t rev;
	uint32_t rsdt;
} __attribute__ ((packed));

struct xsdp {
	char signature[8];
	uint8_t checksum;
	char oem_id[6];
	uint8_t rev;
	uint32_t rsdt;

	uint32_t length;
	uint64_t xsdt;
	uint8_t xchecksum;
	uint8_t reserved[3];
} __attribute__ ((packed));

struct sdt_header {
	char signature[4];
	uint32_t len;
	uint8_t rev;
	uint8_t checksum;
	char oem_id[6];
	char oem_table_id[8];
	uint32_t oem_rev;
	uint32_t creator_id;
	uint32_t creator_rev;
} __attribute__ ((packed));

struct rsdt {
	struct sdt_header header;
	uint32_t entries[];
} __attribute__ ((packed));

struct madt_entry_header {
	uint8_t type;
	uint8_t length;
} __attribute__((packed));

struct madt_local_apic {
	uint8_t type;
	uint8_t length;
	uint8_t processor_id;
	uint8_t apic_id;
	uint32_t flags;
} __attribute__((packed));

struct madt_ioapic {
	uint8_t type;
	uint8_t length;
	uint8_t ioapic_id;
	uint8_t reserved;
	uint32_t ioapic_addr;
	uint32_t gsi_base;
} __attribute__((packed));

struct madt_iso {
	uint8_t type;
	uint8_t length;
	uint8_t bus_source;
	uint8_t irq_source;
	uint32_t gsi;
	uint16_t flags;
} __attribute__((packed));

struct madt_nmi {
	uint8_t type;
	uint8_t length;
	uint8_t processor_id;
	uint16_t flags;
	uint8_t lint;
} __attribute__((packed));

struct madt {
	char signature[4];
	uint32_t len;
	uint8_t rev;
	uint8_t checksum;
	char oem_id[6];
	char oem_table_id[8];
	uint32_t oem_rev;
	uint32_t creator_id;
	uint32_t creator_rev;

	uint32_t lapic_addr;
	uint32_t flags;
	uint8_t entries[];
} __attribute__ ((packed));

uint8_t acpi_isa_irq_gsi(uint8_t isa_irq) {
	if (isa_irq >= ISA_IRQ_COUNT || !isa_irq_overrides[isa_irq].present) {
		return isa_irq;
	}
	return isa_irq_overrides[isa_irq].gsi;
}

bool acpi_isa_irq_active_low(uint8_t isa_irq) {
	if (isa_irq >= ISA_IRQ_COUNT || !isa_irq_overrides[isa_irq].present) {
		return false;
	}
	return isa_irq_overrides[isa_irq].active_low;
}

bool acpi_isa_irq_level_triggered(uint8_t isa_irq) {
	if (isa_irq >= ISA_IRQ_COUNT || !isa_irq_overrides[isa_irq].present) {
		return false;
	}
	return isa_irq_overrides[isa_irq].level_triggered;
}

static void record_iso(struct madt_iso *iso) {
	if (iso->irq_source >= ISA_IRQ_COUNT) {
		return;
	}

	uint8_t polarity = iso->flags & 0x3;
	uint8_t trigger = (iso->flags >> 2) & 0x3;

	isa_irq_override_t *ov = &isa_irq_overrides[iso->irq_source];
	ov->present = true;
	ov->gsi = (uint8_t)iso->gsi;
	ov->active_low = (polarity == 0x3);
	ov->level_triggered = (trigger == 0x3);
}

void *find_table(struct rsdt *rsdt, const char *sig) {
	int num_entries = (rsdt->header.len - sizeof(struct sdt_header)) / 4;

	for (int i = 0; i < num_entries; i++) {
		struct sdt_header *header = (struct sdt_header *)phys_virt(rsdt->entries[i]);
		if (!strncmp(header->signature, sig, strlen(sig))) {
			return (void *)header;
		}
	}
	return NULL;
}

void parse_madt_entries(struct madt *madt) {
	uint8_t *p = madt->entries;
	uint8_t *end = (uint8_t *)madt + madt->len;

	while (p < end) {
		struct madt_entry_header *eh = (struct madt_entry_header *)p;

		if (eh->length == 0) {
			log_debug("MADT: zero-length entry, aborting parse\n");
			break;
		}

		switch (eh->type) {
			case 0: {
				struct madt_local_apic *lapic = (struct madt_local_apic *)p;
				log_debug("MADT: Local APIC - proc_id: %d, apic_id: %d, flags: 0x%X\n",
				      lapic->processor_id, lapic->apic_id, lapic->flags);
				break;
			}
			case 1: {
				struct madt_ioapic *ioapic = (struct madt_ioapic *)p;
				log_debug("MADT: IOAPIC - id: %d, addr: 0x%X, gsi_base: %d\n",
				      ioapic->ioapic_id, ioapic->ioapic_addr, ioapic->gsi_base);
				ioapic_addr = ioapic->ioapic_addr;
				break;
			}
			case 2: {
				struct madt_iso *iso = (struct madt_iso *)p;
				log_debug("MADT: Interrupt Source Override - bus: %d, irq: %d, gsi: %d, flags: 0x%X\n",
				      iso->bus_source, iso->irq_source, iso->gsi, iso->flags);
				record_iso(iso);
				break;
			}
			case 4: {
				struct madt_nmi *nmi = (struct madt_nmi *)p;
				log_debug("MADT: NMI - proc_id: %d, flags: 0x%X, lint: %d\n",
				      nmi->processor_id, nmi->flags, nmi->lint);
				break;
			}
			default:
				log_debug("MADT: unhandled entry type %d, len %d\n", eh->type, eh->length);
				break;
		}

		p += eh->length;
	}
}

void acpi_parse_tables() {
	uint8_t acpi_rev = rsdp_request.response->revision;
	void *rsdp_addr = rsdp_request.response->address;
	struct xsdp *rsdp = (struct xsdp *)phys_virt(frame_alloc());

	log_debug("Filling struct\n");
	memcpy(rsdp, rsdp_addr, acpi_rev > 0 ? sizeof(struct xsdp) : sizeof(struct xsdp) - 16);

	log_debug("RSDP_ADDR: 0x%X\nACPI_REV: %d, RSDP_SIG: %s\nRSDT_ADDR: 0x%X\n", rsdp_addr, acpi_rev, rsdp->signature, rsdp->rsdt);

	struct sdt_header temp_header;
	memcpy(&temp_header, (void *)phys_virt(rsdp->rsdt), sizeof(struct sdt_header));
	struct rsdt *rsdt = (struct rsdt *)phys_virt(frame_alloc());
	memcpy(rsdt, (void *)phys_virt((uint64_t)rsdp->rsdt), temp_header.len);

	void *madt_addr = find_table(rsdt, "APIC");
	if (!madt_addr) {
		log_debug("MADT not found\n");
		return;
	}

	struct sdt_header *madt_hdr = (struct sdt_header *)madt_addr;
	uint32_t madt_len = madt_hdr->len;

	if (madt_len > 0x1000) {
		log_debug("MADT too large for one frame (%d bytes), aborting\n", madt_len);
		return;
	}

	struct madt *madt = (struct madt *)phys_virt(frame_alloc());
	memcpy(madt, madt_addr, madt_len);

	log_debug("MADT_ADDR: 0x%X, LAPIC_ADDR: 0x%X, FLAGS: 0x%X\n", madt_addr, madt->lapic_addr, madt->flags);
	lapic_addr = madt->lapic_addr;

	parse_madt_entries(madt);
	log_debug("APIC_ADDR: 0x%X, IOAPIC_ADDR: 0x%X\n", lapic_addr, ioapic_addr);
}