#include <stdint.h>
#include <stddef.h>
#include <memory.h>
#include <mm/hhdm.h>
#include <mm/paging.h>
#include <acpi.h>
#include <logging/print.h>

uint64_t lapic_addr = 0;
uint64_t ioapic_addr = 0;

static uint32_t ioapic_gsi_base = 0;

// ISA IRQ -> GSI overrides parsed from MADT type-2 entries. Stored for later
// use when routing device IRQs; not applied yet.
struct irq_override {
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags;
};

static struct irq_override overrides[16];
static uint8_t override_count = 0;

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
} __attribute__((packed));

// Root table: RSDT holds 32-bit entry pointers, XSDT 64-bit.
struct rsdt {
    struct sdt_header header;
    uint32_t entries[];
} __attribute__((packed));

struct xsdt {
    struct sdt_header header;
    uint64_t entries[];
} __attribute__((packed));

struct madt {
    struct sdt_header header;
    uint32_t local_apic_addr;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

struct madt_entry_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

static void parse_madt(struct madt *madt) {
    lapic_addr = madt->local_apic_addr;

    uintptr_t end = (uintptr_t)madt + madt->header.len;
    uint8_t *p = madt->entries;

    while ((uintptr_t)p < end) {
        struct madt_entry_header *eh = (struct madt_entry_header *)p;

        switch (eh->type) {
        case 0: { // Local APIC
            // p+2: proc_id, p+3: apic_id, p+4: flags — logged only for now
            break;
        }
        case 1: { // IO APIC: id(1) reserved(1) addr(4) gsi_base(4)
            // First IOAPIC wins; multiple-IOAPIC routing is out of scope.
            if (!ioapic_addr) {
                ioapic_addr = *(uint32_t *)(p + 4);
                ioapic_gsi_base = *(uint32_t *)(p + 8);
            }
            break;
        }
        case 2: { // Interrupt Source Override: bus(1) irq(1) gsi(4) flags(2)
            if (override_count < sizeof(overrides) / sizeof(overrides[0])) {
                overrides[override_count].irq   = *(p + 3);
                overrides[override_count].gsi   = *(uint32_t *)(p + 4);
                overrides[override_count].flags = *(uint16_t *)(p + 8);
                override_count++;
            }
            break;
        }
        case 4: { // Local APIC NMI — accepted, unused
            break;
        }
        default:
            // Unknown entry types are skipped via their length field.
            break;
        }

        // Zero-length entries would loop forever; bail instead of hanging.
        if (!eh->length) {
            print("MADT: zero-length entry, aborting parse");
            return;
        }
        p += eh->length;
    }
}

static struct sdt_header *find_table(uintptr_t root_phys, int use_xsdt) {
    size_t header_bytes = use_xsdt ? offsetof(struct xsdt, entries)
                                   : offsetof(struct rsdt, entries);
    void *root = phys_to_virt(root_phys);
    struct sdt_header *root_hdr = (struct sdt_header *)root;
    size_t entry_size = use_xsdt ? 8 : 4;
    size_t count = (root_hdr->len - header_bytes) / entry_size;

    for (size_t i = 0; i < count; i++) {
        uintptr_t entry_phys = use_xsdt
            ? (uintptr_t)((struct xsdt *)root)->entries[i]
            : (uintptr_t)((struct rsdt *)root)->entries[i];

        struct sdt_header *hdr = (struct sdt_header *)phys_to_virt(entry_phys);
        if (hdr->signature[0] == 'A' && hdr->signature[1] == 'P' &&
            hdr->signature[2] == 'I' && hdr->signature[3] == 'C') {
            return hdr;
        }
    }
    return NULL;
}

uint8_t acpi_init(uint64_t rsdp) {
    if (!rsdp) {
        print("ACPI: no RSDP provided by bootloader");
        return 1;
    }

    // Limine's rsdp response gives an HHDM-virtual pointer, unlike the
    // physical table pointers stored inside the XSDT/RSDT.
    // RSDP rev >= 2 carries an XSDT pointer; older ones only an RSDT.
    uint8_t acpi_rev = *(uint8_t *)(rsdp + 15);

    struct madt *madt;
    if (acpi_rev >= 2) {
        uintptr_t xsdt_phys = *(uint64_t *)(rsdp + 24);
        madt = (struct madt *)find_table(xsdt_phys, 1);
    } else {
        uintptr_t rsdt_phys = *(uint32_t *)(rsdp + 16);
        madt = (struct madt *)find_table(rsdt_phys, 0);
    }

    if (!madt) {
        print("ACPI: MADT not found");
        return 1;
    }

    parse_madt(madt);

    if (!lapic_addr || !ioapic_addr) {
        print("ACPI: MADT missing LAPIC/IOAPIC addresses");
        return 1;
    }

    print("ACPI: MADT found, LAPIC @ 0x%lx, IOAPIC @ 0x%lx (gsi base %u)",
          lapic_addr, ioapic_addr, ioapic_gsi_base);
    print("ACPI: %u IRQ overrides", override_count);

    // Both are page-aligned MMIO regions; map them uncached through the HHDM.
    // kernel_pml4 is physical, so wrap it before handing it to map_page().
    uint64_t *pml4 = (uint64_t *)phys_to_virt((uintptr_t)kernel_pml4);
    uint64_t flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_DISABLE_CACHE | PAGE_NXE;

    if (map_page(pml4, phys_to_virt(lapic_addr), lapic_addr, flags)) return 1;
    if (map_page(pml4, phys_to_virt(ioapic_addr), ioapic_addr, flags)) return 1;

    return 0;
}
