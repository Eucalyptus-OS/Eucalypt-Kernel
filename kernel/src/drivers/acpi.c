#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/framebuffer.h>
#include <limine.h>
#include <logging/printk.h>
#include <mem.h>
#include <mm/hhdm.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0
};

struct acpi_rsdp {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_bgrt {
    struct acpi_sdt_header header;
    uint16_t version;
    uint8_t  status;
    uint8_t  image_type;
    uint64_t image_address;
    uint32_t image_offset_x;
    uint32_t image_offset_y;
} __attribute__((packed));

struct bmp_file_header {
    uint16_t signature;
    uint32_t file_size;
    uint16_t reserved0;
    uint16_t reserved1;
    uint32_t data_offset;
} __attribute__((packed));

struct bmp_info_header {
    uint32_t header_size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bits_per_pixel;
    uint32_t compression;
    uint32_t image_size;
} __attribute__((packed));

static uint8_t acpi_checksum(const void *table, size_t length) {
    const uint8_t *bytes = table;
    uint8_t sum = 0;

    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }

    return sum;
}

static bool acpi_sig_eq(const char signature[4], const char *expected) {
    return memcmp(signature, expected, 4) == 0;
}

static void acpi_sig_string(const char signature[4], char out[5]) {
    memcpy(out, signature, 4);
    out[4] = '\0';
}

static const char *bgrt_orientation(uint8_t status) {
    switch ((status >> 1) & 0x3) {
        case 0: return "0deg";
        case 1: return "90deg";
        case 2: return "180deg";
        case 3: return "270deg";
    }

    return "unknown";
}

static const char *bgrt_image_type(uint8_t image_type) {
    switch (image_type) {
        case 0: return "BMP";
    }

    return "unknown";
}

static uint32_t abs_i32(int32_t value) {
    return value < 0 ? (uint32_t)-value : (uint32_t)value;
}

static uint32_t bmp_row_stride(uint32_t width, uint16_t bits_per_pixel) {
    return ((width * bits_per_pixel + 31) / 32) * 4;
}

static uint8_t acpi_draw_bgrt_bmp(const struct acpi_bgrt *bgrt,
                                  const struct bmp_file_header *file,
                                  const struct bmp_info_header *info) {
    if (info->compression != 0
        || (info->bits_per_pixel != 24 && info->bits_per_pixel != 32)) {
        log_warn("ACPI: BGRT BMP draw unsupported: compression %u bpp %u\n",
                 info->compression, info->bits_per_pixel);
        return 1;
    }

    framebuffer_info_t fb;
    if (framebuffer_get_info(0, &fb) != 0) {
        log_warn("ACPI: no framebuffer available for BGRT draw\n");
        return 1;
    }

    uint32_t width = abs_i32(info->width);
    uint32_t height = abs_i32(info->height);
    uint32_t stride = bmp_row_stride(width, info->bits_per_pixel);
    uint32_t bytes_per_pixel = info->bits_per_pixel / 8;
    bool top_down = info->height < 0;
    const uint8_t *pixel_data = (const uint8_t *)file + file->data_offset;

    for (uint32_t y = 0; y < height; y++) {
        uint64_t dst_y = (uint64_t)bgrt->image_offset_y + y;
        if (dst_y >= fb.height) {
            break;
        }

        uint32_t src_y = top_down ? y : height - 1 - y;
        const uint8_t *row = pixel_data + src_y * stride;

        for (uint32_t x = 0; x < width; x++) {
            uint64_t dst_x = (uint64_t)bgrt->image_offset_x + x;
            if (dst_x >= fb.width) {
                break;
            }

            const uint8_t *pixel = row + x * bytes_per_pixel;
            uint32_t color = framebuffer_rgb(0, pixel[2], pixel[1], pixel[0]);
            framebuffer_put_pixel(0, dst_x, dst_y, color);
        }
    }

    return 0;
}

static void acpi_log_bgrt_bmp(const struct acpi_bgrt *bgrt) {
    if (bgrt->image_type != 0 || bgrt->image_address == 0) {
        return;
    }

    const struct bmp_file_header *file =
        (const struct bmp_file_header *)phys_virt(bgrt->image_address);
    if (file->signature != 0x4d42) {
        log_warn("ACPI: BGRT image is not a BMP: signature 0x%X\n",
                 file->signature);
        return;
    }

    const struct bmp_info_header *info =
        (const struct bmp_info_header *)((const uint8_t *)file + sizeof(*file));
    if (info->header_size < 40 || info->planes != 1) {
        log_warn("ACPI: BGRT BMP header is unsupported: size %u planes %u\n",
                 info->header_size, info->planes);
        return;
    }

    log_info("ACPI: BGRT BMP %ux%u %u bpp data offset %u size %u\n",
             abs_i32(info->width), abs_i32(info->height),
             info->bits_per_pixel, file->data_offset, file->file_size);

    if (acpi_draw_bgrt_bmp(bgrt, file, info) == 0) {
        log_info("ACPI: BGRT image drawn to framebuffer 0\n");
    }
}

static void acpi_log_bgrt(const struct acpi_bgrt *bgrt) {
    bool displayed = (bgrt->status & 1) != 0;

    log_info("ACPI: BGRT version %u status %u displayed %s orientation %s\n",
             bgrt->version, bgrt->status, displayed ? "yes" : "no",
             bgrt_orientation(bgrt->status));
    log_info("ACPI: BGRT image type %u (%s) address %llX offset %u,%u\n",
             bgrt->image_type, bgrt_image_type(bgrt->image_type),
             (unsigned long long)bgrt->image_address,
             bgrt->image_offset_x, bgrt->image_offset_y);
    acpi_log_bgrt_bmp(bgrt);
}

static void acpi_log_table(uint64_t table_phys, bool *found_bgrt) {
    struct acpi_sdt_header *header = (void *)phys_virt(table_phys);
    char signature[5];

    if (!table_phys) {
        return;
    }

    acpi_sig_string(header->signature, signature);
    log_info("ACPI: table %s at %llX length %u revision %u\n",
             signature, (unsigned long long)table_phys,
             header->length, header->revision);

    if (header->length < sizeof(*header)) {
        log_warn("ACPI: table %s has invalid length %u\n",
                 signature, header->length);
        return;
    }

    if (acpi_checksum(header, header->length) != 0) {
        log_warn("ACPI: table %s checksum failed\n", signature);
    }

    if (!*found_bgrt && acpi_sig_eq(header->signature, "BGRT")) {
        if (header->length < sizeof(struct acpi_bgrt)) {
            log_warn("ACPI: BGRT table too short: %u\n", header->length);
            return;
        }

        acpi_log_bgrt((const struct acpi_bgrt *)header);
        *found_bgrt = true;
    }
}

void acpi_log_tables(void) {
    struct limine_rsdp_response *response = rsdp_request.response;

    if (!response || !response->address) {
        log_warn("ACPI: Limine did not provide an RSDP\n");
        return;
    }

    const struct acpi_rsdp *rsdp = response->address;
    char oem_id[7];
    memcpy(oem_id, rsdp->oem_id, 6);
    oem_id[6] = '\0';

    log_info("ACPI: RSDP at %llX OEM %s revision %u\n",
             (unsigned long long)(uintptr_t)rsdp, oem_id, rsdp->revision);

    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        log_warn("ACPI: RSDP signature is invalid\n");
        return;
    }

    if (acpi_checksum(rsdp, 20) != 0) {
        log_warn("ACPI: RSDP checksum failed\n");
    }

    if (rsdp->revision >= 2 && rsdp->length >= sizeof(*rsdp)
        && acpi_checksum(rsdp, rsdp->length) != 0) {
        log_warn("ACPI: XSDP checksum failed\n");
    }

    bool use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_address != 0;
    uint64_t root_phys = use_xsdt ? rsdp->xsdt_address : rsdp->rsdt_address;

    if (!root_phys) {
        log_warn("ACPI: no RSDT/XSDT address present\n");
        return;
    }

    struct acpi_sdt_header *root = (void *)phys_virt(root_phys);
    uint32_t entry_size = use_xsdt ? sizeof(uint64_t) : sizeof(uint32_t);

    if (root->length < sizeof(*root)) {
        log_warn("ACPI: root table length is invalid: %u\n", root->length);
        return;
    }

    uint32_t entry_count = (root->length - sizeof(*root)) / entry_size;
    log_info("ACPI: using %s at %llX with %u entries\n",
             use_xsdt ? "XSDT" : "RSDT",
             (unsigned long long)root_phys, entry_count);

    if (acpi_checksum(root, root->length) != 0) {
        log_warn("ACPI: root table checksum failed\n");
    }

    bool found_bgrt = false;
    uint8_t *entries = (uint8_t *)root + sizeof(*root);

    for (uint32_t i = 0; i < entry_count; i++) {
        uint64_t table_phys;

        if (use_xsdt) {
            table_phys = ((uint64_t *)entries)[i];
        } else {
            table_phys = ((uint32_t *)entries)[i];
        }

        acpi_log_table(table_phys, &found_bgrt);
    }

    if (!found_bgrt) {
        log_info("ACPI: BGRT not present\n");
    }
}
