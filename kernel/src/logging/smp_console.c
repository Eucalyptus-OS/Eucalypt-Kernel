#include <stdint.h>
#include <stdbool.h>
#include <limine.h>
#include <logging/smp_console.h>

#define MAX_CPUS 24

extern volatile struct limine_framebuffer_request framebuffer_request;
extern uint8_t font[26][8];
extern uint8_t font_lower[26][8];

#define GLYPH_W 8
#define GLYPH_H 8

struct console {
    uint16_t row;
    uint16_t col;
    uint32_t color;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t width_px;
    uint32_t height_px;
    uint16_t max_rows;
    uint16_t max_cols;
    bool active;
};

static struct console consoles[MAX_CPUS];
static uint8_t console_count = 0;
static uint16_t grid_cols = 0;
static uint16_t grid_rows = 0;

static inline struct limine_framebuffer *get_fb(void) {
    return framebuffer_request.response->framebuffers[0];
}

static void compute_grid_dims(uint8_t n, uint16_t *out_cols, uint16_t *out_rows) {
    uint16_t best_cols = n;
    uint16_t best_rows = 1;
    uint32_t best_waste = (uint32_t)best_cols * best_rows - n;
    int32_t  best_aspect_diff = (int32_t)best_cols - (int32_t)best_rows;
    if (best_aspect_diff < 0) best_aspect_diff = -best_aspect_diff;

    for (uint16_t cols = 1; cols <= n; cols++) {
        uint16_t rows = (n + cols - 1) / cols;
        uint32_t waste = (uint32_t)cols * rows - n;
        int32_t aspect_diff = (int32_t)cols - (int32_t)rows;
        if (aspect_diff < 0) aspect_diff = -aspect_diff;

        if (waste < best_waste ||
            (waste == best_waste && aspect_diff < best_aspect_diff)) {
            best_waste = waste;
            best_aspect_diff = aspect_diff;
            best_cols = cols;
            best_rows = rows;
        }
    }

    *out_cols = best_cols;
    *out_rows = best_rows;
}

void smp_console_new_line(uint8_t cpu_id) {
    struct console *c = &consoles[cpu_id];
    c->row++;
    c->col = 0;
    if (c->row >= c->max_rows) {
        c->row = 0;
    }
}

void smp_console_draw_glyph(uint8_t cpu_id, uint32_t color, char c) {
    uint8_t glyph;
    bool upper_case;
    struct console *con = &consoles[cpu_id];

    if (!con->active) return;

    struct limine_framebuffer *fbinfo = get_fb();
    uint32_t *fb = (uint32_t *)fbinfo->address;
    uint32_t stride = fbinfo->pitch / 4;

    if (c >= 'A' && c <= 'Z') {
        upper_case = true;
        glyph = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
        upper_case = false;
        glyph = c - 'a';
    } else if (c == ' ') {
        con->col++;
        if (con->col >= con->max_cols) smp_console_new_line(cpu_id);
        return;
    } else if (c == '\n') {
        smp_console_new_line(cpu_id);
        return;
    } else {
        return;
    }

    uint32_t origin_x = con->origin_x + (con->col * GLYPH_W);
    uint32_t origin_y = con->origin_y + (con->row * GLYPH_H);

    for (uint8_t i = 0; i < GLYPH_H; i++) {
        uint8_t row_bits = upper_case ? font[glyph][i] : font_lower[glyph][i];
        for (uint8_t j = 0; j < GLYPH_W; j++) {
            uint32_t px = origin_x + j;
            uint32_t py = origin_y + i;
            fb[py * stride + px] = (row_bits & (1 << (7 - j))) ? color : 0x00000000;
        }
    }

    con->col++;
    if (con->col >= con->max_cols) {
        smp_console_new_line(cpu_id);
    }
}

uint8_t smp_console_init(uint8_t cpu_count) {
    if (cpu_count == 0 || cpu_count > MAX_CPUS) {
        return 1;
    }

    struct limine_framebuffer *fb = get_fb();

    compute_grid_dims(cpu_count, &grid_cols, &grid_rows);

    uint32_t region_w = fb->width  / grid_cols;
    uint32_t region_h = fb->height / grid_rows;

    for (uint8_t i = 0; i < cpu_count; i++) {
        uint16_t gx = i % grid_cols;
        uint16_t gy = i / grid_cols;

        consoles[i].origin_x  = gx * region_w;
        consoles[i].origin_y  = gy * region_h;
        consoles[i].width_px  = region_w;
        consoles[i].height_px = region_h;
        consoles[i].max_cols  = region_w / GLYPH_W;
        consoles[i].max_rows  = region_h / GLYPH_H;
        consoles[i].row = 0;
        consoles[i].col = 0;
        consoles[i].color = 0xFFFFFFFF;
        consoles[i].active = true;
    }

    console_count = cpu_count;
    return 0;
}