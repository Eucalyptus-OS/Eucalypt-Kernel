#include <stdint.h>
#include <stdbool.h>
#include <limine.h>
#include <mem.h>
#include <sync/spinlock.h>
#include <logging/smp_console.h>

#define MAX_CPUS 24
#define GRID_GAP_PX 5
#define LETTER_SPACING_PX 1

extern volatile struct limine_framebuffer_request framebuffer_request;
extern uint8_t font[26][8];
extern uint8_t font_lower[26][8];

static spinlock_t console_lock = 0;

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
static bool enabled = false;

static inline struct limine_framebuffer *get_fb(void) {
    return framebuffer_request.response->framebuffers[0];
}

static inline bool smp_console_enabled(void) {
    return __atomic_load_n(&enabled, __ATOMIC_ACQUIRE);
}

static void compute_grid_dims(uint8_t n, uint16_t *out_cols, uint16_t *out_rows) {
    uint16_t best_cols = n;
    uint16_t best_rows = 1;
    uint32_t best_waste = (uint32_t)best_cols * best_rows - n;
    int32_t best_aspect_diff = (int32_t)best_cols - (int32_t)best_rows;
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

static void smp_console_scroll_locked(uint8_t cpu_id) {
    struct console *c = &consoles[cpu_id];
    if (c->height_px <= GLYPH_H) {
        return;
    }

    struct limine_framebuffer *fbinfo = get_fb();
    uint32_t *fb = (uint32_t *)fbinfo->address;
    uint32_t stride = fbinfo->pitch / 4;

    for (uint32_t y = 0; y < c->height_px - GLYPH_H; y++) {
        uint32_t dst_y = c->origin_y + y;
        uint32_t src_y = c->origin_y + y + GLYPH_H;
        memmove(&fb[dst_y * stride + c->origin_x],
                &fb[src_y * stride + c->origin_x],
                c->width_px * sizeof(uint32_t));
    }

    for (uint32_t y = c->height_px - GLYPH_H; y < c->height_px; y++) {
        uint32_t py = c->origin_y + y;
        for (uint32_t x = 0; x < c->width_px; x++) {
            fb[py * stride + c->origin_x + x] = 0xFF000000;
        }
    }
}

static void smp_console_new_line_locked(uint8_t cpu_id) {
    struct console *c = &consoles[cpu_id];
    if (c->max_rows == 0) {
        return;
    }

    c->col = 0;
    if (c->row + 1 >= c->max_rows) {
        smp_console_scroll_locked(cpu_id);
    } else {
        c->row++;
    }
}

void smp_console_new_line(uint8_t cpu_id) {
    if (!smp_console_enabled() || cpu_id >= console_count) {
        return;
    }

    spinlock_acquire(&console_lock);
    smp_console_new_line_locked(cpu_id);
    spinlock_release(&console_lock);
}

void smp_console_draw_glyph(uint8_t cpu_id, uint32_t color, char c) {
    if (!smp_console_enabled() || cpu_id >= console_count) {
        return;
    }
    uint8_t glyph;
    bool upper_case;

    struct limine_framebuffer *fbinfo = get_fb();
    uint32_t *fb = (uint32_t *)fbinfo->address;
    uint32_t stride = fbinfo->pitch / 4;
    spinlock_acquire(&console_lock);

    struct console *con = &consoles[cpu_id];
    if (!con->active || con->max_cols == 0 || con->max_rows == 0) {
        spinlock_release(&console_lock);
        return;
    }

    uint16_t cell_w = GLYPH_W + LETTER_SPACING_PX;

    if (c >= 'A' && c <= 'Z') {
        upper_case = true;
        glyph = c - 'A';
    } else if (c >= 'a' && c <= 'z') {
        upper_case = false;
        glyph = c - 'a';
    } else if (c == ' ') {
        con->col++;
        if (con->col >= con->max_cols) {
            smp_console_new_line_locked(cpu_id);
        }
        spinlock_release(&console_lock);
        return;
    } else if (c == '\n' || c == '\r') {
        smp_console_new_line_locked(cpu_id);
        spinlock_release(&console_lock);
        return;
    } else {
        spinlock_release(&console_lock);
        return;
    }

    uint32_t origin_x = con->origin_x + (con->col * cell_w);
    uint32_t origin_y = con->origin_y + (con->row * GLYPH_H);

    for (uint8_t i = 0; i < GLYPH_H; i++) {
        uint8_t row_bits = upper_case ? font[glyph][i] : font_lower[glyph][i];
        for (uint8_t j = 0; j < GLYPH_W; j++) {
            uint32_t px = origin_x + j;
            uint32_t py = origin_y + i;
            fb[py * stride + px] = (row_bits & (1 << (7 - j))) ? color : 0x00000000;
        }
        for (uint8_t j = 0; j < LETTER_SPACING_PX; j++) {
            uint32_t px = origin_x + GLYPH_W + j;
            uint32_t py = origin_y + i;
            fb[py * stride + px] = 0x00000000;
        }
    }

    con->col++;
    if (con->col >= con->max_cols) {
        smp_console_new_line_locked(cpu_id);
    }
    spinlock_release(&console_lock);
}

uint8_t smp_console_init(uint8_t cpu_count) {
    if (cpu_count == 0 || cpu_count > MAX_CPUS) {
        return 1;
    }

    struct limine_framebuffer *fb = get_fb();
    uint32_t *fbpix = (uint32_t *)fb->address;
    uint32_t stride = fb->pitch / 4;

    compute_grid_dims(cpu_count, &grid_cols, &grid_rows);

    uint32_t cell_w = fb->width / grid_cols;
    uint32_t cell_h = fb->height / grid_rows;

    uint32_t region_w = (cell_w > 2 * GRID_GAP_PX) ? cell_w - 2 * GRID_GAP_PX : cell_w;
    uint32_t region_h = (cell_h > 2 * GRID_GAP_PX) ? cell_h - 2 * GRID_GAP_PX : cell_h;

    uint16_t letter_cell_w = GLYPH_W + LETTER_SPACING_PX;

    for (uint8_t i = 0; i < cpu_count; i++) {
        uint16_t gx = i % grid_cols;
        uint16_t gy = i / grid_cols;

        consoles[i].origin_x = (gx * cell_w) + GRID_GAP_PX;
        consoles[i].origin_y = (gy * cell_h) + GRID_GAP_PX;
        consoles[i].width_px = region_w;
        consoles[i].height_px = region_h;
        consoles[i].max_cols = region_w / letter_cell_w;
        consoles[i].max_rows = region_h / GLYPH_H;
        consoles[i].row = 0;
        consoles[i].col = 0;
        consoles[i].color = 0xFFFFFFFF;
        consoles[i].active = true;
    }

    console_count = cpu_count;

    for (uint32_t y = 0; y < fb->height; y++) {
        for (uint32_t x = 0; x < fb->width; x++) {
            fbpix[y * stride + x] = 0xFF000000;
        }
    }

    __atomic_store_n(&enabled, true, __ATOMIC_RELEASE);
    
    return 0;
}

void println(uint8_t cpu_id, const char *str, uint32_t color) {
    if (!smp_console_enabled()) {
        return;
    }
    for (uint8_t i = 0; str[i] != '\0'; i++) {
        smp_console_draw_glyph(cpu_id, color, str[i]);
    }
    smp_console_new_line(cpu_id);
}
