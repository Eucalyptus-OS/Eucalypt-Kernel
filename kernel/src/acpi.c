#include <stddef.h>
#include <limine.h>
#include <mm/frame.h>
#include <mm/memory.h>
#include <mm/hhdm.h>
#include <logging/print.h>
#include <acpi.h>

uint64_t lapic_addr = 0;
uint64_t ioapic_addr = 0;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
	.id = LIMINE_RSDP_REQUEST_ID,
	.revision = 0
};

// ACPI 1.0 RSDP: an XSDP without the extended v2 fields
struct rsdp {
	char signature[8];   // ASCII "RSD PTR "
	uint8_t checksum;
	char oem_id[6];
	uint8_t rev;         // >= 2 signals ACPI 2.0, where the RSDT pointer gives way to an XSDT
	uint32_t rsdt;
} __attribute__ ((packed));

struct xsdp {
	char signature[8];
	uint8_t checksum;
	char oem_id[6];
	uint8_t rev;
	uint32_t rsdt;

	// ACPI 2.0 additions: full length and the 64-bit XSDT pointer
	uint32_t length;
	uint64_t xsdt;       // 8-byte root table pointer, supports addresses above 4 GiB
	uint8_t xchecksum;
	uint8_t reserved[3];
} __attribute__ ((packed));

struct sdt_header {
	char signature[4];   // 4-byte ASCII table signature, e.g. "RSDT" or "APIC"
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
	uint8_t type;      // 0
	uint8_t length;
	uint8_t processor_id;
	uint8_t apic_id;
	uint32_t flags;
} __attribute__((packed));

struct madt_ioapic {
	uint8_t type;      // 1
	uint8_t length;
	uint8_t ioapic_id;
	uint8_t reserved;
	uint32_t ioapic_addr;
	uint32_t gsi_base;
} __attribute__((packed));

struct madt_iso {
	uint8_t type;      // 2, interrupt source override
	uint8_t length;
	uint8_t bus_source;
	uint8_t irq_source;
	uint32_t gsi;
	uint16_t flags;
} __attribute__((packed));

struct madt_nmi {
	uint8_t type;      // 4, non-maskable interrupts
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

// Search the RSDT for a table whose signature matches, returning its virtual address
void *find_table(struct rsdt *rsdt, const char *sig) {
	int num_entries = (rsdt->header.len - sizeof(struct sdt_header)) / 4;

	for (int i = 0; i < num_entries; i++) {
		// RSDT entries hold 32-bit physical addresses, so map them before reading
		struct sdt_header *header = (struct sdt_header *)phys_to_virt(rsdt->entries[i]);
		if (!strncmp(header->signature, sig, strlen(sig))) {
			return (void *)header;
		}
	}
	return NULL;
}

// Walk the MADT's variable-length entries and log each APIC structure found
void parse_madt_entries(struct madt *madt) {
	uint8_t *p = madt->entries;
	uint8_t *end = (uint8_t *)madt + madt->len;

	while (p < end) {
		struct madt_entry_header *eh = (struct madt_entry_header *)p;

		if (eh->length == 0) {
			print("MADT: zero-length entry, aborting parse\n");
			break;
		}

		switch (eh->type) {
			case 0: {
				struct madt_local_apic *lapic = (struct madt_local_apic *)p;
				print("MADT: Local APIC - proc_id: %d, apic_id: %d, flags: 0x%X\n",
				      lapic->processor_id, lapic->apic_id, lapic->flags);
				break;
			}
			case 1: {
				struct madt_ioapic *ioapic = (struct madt_ioapic *)p;
				print("MADT: IOAPIC - id: %d, addr: 0x%X, gsi_base: %d\n",
				      ioapic->ioapic_id, ioapic->ioapic_addr, ioapic->gsi_base);
				// Keep the first I/O APIC's MMIO base for APIC setup later
				ioapic_addr = ioapic->ioapic_addr;
				break;
			}
			case 2: {
				struct madt_iso *iso = (struct madt_iso *)p;
				print("MADT: Interrupt Source Override - bus: %d, irq: %d, gsi: %d, flags: 0x%X\n",
				      iso->bus_source, iso->irq_source, iso->gsi, iso->flags);
				break;
			}
			case 4: {
				struct madt_nmi *nmi = (struct madt_nmi *)p;
				print("MADT: NMI - proc_id: %d, flags: 0x%X, lint: %d\n",
				      nmi->processor_id, nmi->flags, nmi->lint);
				break;
			}
			default:
				print("MADT: unhandled entry type %d, len %d\n", eh->type, eh->length);
				break;
		}

		p += eh->length;
	}
}

// Parse RSDP/RSDT to locate the MADT, then record the LAPIC and IOAPIC base addresses
void acpi_parse_tables() {
	// Copy the physical RSDP into a mapped frame so we can safely read it
	void *rsdp_addr = rsdp_request.response->address;
	struct xsdp *rsdp = (struct xsdp *)phys_to_virt(frame_alloc());

	print("Filling struct\n");
	// Byte 15 holds the revision; v2+ RSDPs carry the extra XSDT fields to copy
	uint8_t acpi_rev = *(uint8_t *)((char *)rsdp_addr + 15);
	size_t rsdp_size = acpi_rev >= 2 ? sizeof(struct xsdp) : sizeof(struct xsdp) - 16;
	memcpy(rsdp, rsdp_addr, rsdp_size);

	print("RSDP_ADDR: 0x%X\nACPI_REV: %d, RSDP_SIG: %s\nRSDT_ADDR: 0x%X\n", rsdp_addr, acpi_rev, rsdp->signature, rsdp->rsdt);

	// Peek at the header first to learn the RSDT's total length
	struct sdt_header temp_header;
	memcpy(&temp_header, (void *)phys_to_virt(rsdp->rsdt), sizeof(struct sdt_header));
	// Copy the whole RSDT into a mapped frame so its entries can be dereferenced
	struct rsdt *rsdt = (struct rsdt *)phys_to_virt(frame_alloc());
	memcpy(rsdt, phys_to_virt(rsdp->rsdt), temp_header.len);

	void *madt_addr = find_table(rsdt, "APIC");
	if (!madt_addr) {
		print("MADT not found\n");
		return;
	}

	struct sdt_header *madt_hdr = (struct sdt_header *)madt_addr;
	uint32_t madt_len = madt_hdr->len;

	// Refuse tables that will not fit in the single 4 KiB frame we copy them into
	if (madt_len > 0x1000) {
		print("MADT too large for one frame (%d bytes), aborting\n", madt_len);
		return;
	}

	struct madt *madt = (struct madt *)phys_to_virt(frame_alloc());
	memcpy(madt, madt_addr, madt_len);

	print("MADT_ADDR: 0x%X, LAPIC_ADDR: 0x%X, FLAGS: 0x%X\n", madt_addr, madt->lapic_addr, madt->flags);
	// Save the memory-mapped local APIC base reported by the MADT
	lapic_addr = madt->lapic_addr;

	parse_madt_entries(madt);
	print("APIC_ADDR: 0x%X, IOAPIC_ADDR: 0x%X\n", lapic_addr, ioapic_addr);
}