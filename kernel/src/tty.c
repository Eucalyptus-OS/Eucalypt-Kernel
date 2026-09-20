#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <portio.h>
#include <mm/memory.h>
#include <mm/heap.h>
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <tty.h>
#include <logging/print.h>
#include <multitasking/sched.h>
#include <multitasking/proc.h>
#include <multitasking/thread.h>
#include <abi/errno.h>

#define GLYPH_W 8
#define GLYPH_H 8
#define LETTER_SPACING_PX 1

extern volatile struct limine_framebuffer_request framebuffer_request;

static tty_t   ttys[TTY_MAX];
static uint8_t active_tty = 0;
static void  (*global_output_fn)(tty_t *tty, char c) = NULL;

static uint32_t *tty_fb(tty_t *tty) {
    return tty->backbuf;
}

static uint32_t tty_fb_stride(void) {
    return framebuffer_request.response->framebuffers[0]->pitch / 4;
}

static void tty_blit(tty_t *tty);
static void tty_blit_region(tty_t *tty, uint32_t start_row, uint32_t end_row);
static void tty_fill_cell(tty_t *tty, uint32_t row, uint32_t col, uint32_t color);

static uint8_t tty_font[128][8] = {
    ['!'] = { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00 },
    ['"'] = { 0x66, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00 },
    ['#'] = { 0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00 },
    ['$'] = { 0x18, 0x3E, 0x60, 0x3C, 0x06, 0x7C, 0x18, 0x00 },
    ['%'] = { 0x00, 0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00 },
    ['&'] = { 0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00 },
    ['\''] = { 0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00 },
    ['('] = { 0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00 },
    [')'] = { 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00 },
    ['*'] = { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 },
    ['+'] = { 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00 },
    [','] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30 },
    ['-'] = { 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00 },
    ['.'] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00 },
    ['/'] = { 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00 },
    ['0'] = { 0x7C, 0xC6, 0xCE, 0xDE, 0xF6, 0xE6, 0x7C, 0x00 },
    ['1'] = { 0x18, 0x38, 0x78, 0x18, 0x18, 0x18, 0x7E, 0x00 },
    ['2'] = { 0x7C, 0xC6, 0x06, 0x1C, 0x70, 0xC0, 0xFE, 0x00 },
    ['3'] = { 0x7C, 0xC6, 0x06, 0x3C, 0x06, 0xC6, 0x7C, 0x00 },
    ['4'] = { 0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x1E, 0x00 },
    ['5'] = { 0xFE, 0xC0, 0xFC, 0x06, 0x06, 0xC6, 0x7C, 0x00 },
    ['6'] = { 0x38, 0x60, 0xC0, 0xFC, 0xC6, 0xC6, 0x7C, 0x00 },
    ['7'] = { 0xFE, 0xC6, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 },
    ['8'] = { 0x7C, 0xC6, 0xC6, 0x7C, 0xC6, 0xC6, 0x7C, 0x00 },
    ['9'] = { 0x7C, 0xC6, 0xC6, 0x7E, 0x06, 0x0C, 0x78, 0x00 },
    [':'] = { 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x00 },
    [';'] = { 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30 },
    ['<'] = { 0x0E, 0x1C, 0x38, 0x70, 0x38, 0x1C, 0x0E, 0x00 },
    ['='] = { 0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00 },
    ['>'] = { 0x70, 0x38, 0x1C, 0x0E, 0x1C, 0x38, 0x70, 0x00 },
    ['?'] = { 0x7C, 0xC6, 0x0C, 0x18, 0x18, 0x00, 0x18, 0x00 },
    ['@'] = { 0x7C, 0xC6, 0xDE, 0xDE, 0xDE, 0xC0, 0x78, 0x00 },
    ['A'] = { 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00 },
    ['B'] = { 0xFC, 0x66, 0x66, 0x7C, 0x66, 0x66, 0xFC, 0x00 },
    ['C'] = { 0x3C, 0x66, 0xC0, 0xC0, 0xC0, 0x66, 0x3C, 0x00 },
    ['D'] = { 0xF8, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00 },
    ['E'] = { 0xFE, 0x62, 0x68, 0x78, 0x68, 0x62, 0xFE, 0x00 },
    ['F'] = { 0xFE, 0x62, 0x68, 0x78, 0x68, 0x60, 0xF0, 0x00 },
    ['G'] = { 0x3C, 0x66, 0xC0, 0xC0, 0xCE, 0x66, 0x3E, 0x00 },
    ['H'] = { 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 },
    ['I'] = { 0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 },
    ['J'] = { 0x1E, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78, 0x00 },
    ['K'] = { 0xE6, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0xE6, 0x00 },
    ['L'] = { 0xF0, 0x60, 0x60, 0x60, 0x62, 0x66, 0xFE, 0x00 },
    ['M'] = { 0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xC6, 0xC6, 0x00 },
    ['N'] = { 0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00 },
    ['O'] = { 0x38, 0x6C, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00 },
    ['P'] = { 0xFC, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00 },
    ['Q'] = { 0x38, 0x6C, 0xC6, 0xC6, 0xD6, 0xCC, 0x76, 0x00 },
    ['R'] = { 0xFC, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0xE6, 0x00 },
    ['S'] = { 0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00 },
    ['T'] = { 0x7E, 0x5A, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 },
    ['U'] = { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 },
    ['V'] = { 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 },
    ['W'] = { 0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00 },
    ['X'] = { 0xC6, 0x6C, 0x38, 0x38, 0x38, 0x6C, 0xC6, 0x00 },
    ['Y'] = { 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x3C, 0x00 },
    ['Z'] = { 0xFE, 0xC6, 0x8C, 0x18, 0x32, 0x66, 0xFE, 0x00 },
    ['['] = { 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00 },
    ['\\'] = { 0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00 },
    [']'] = { 0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00 },
    ['^'] = { 0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00 },
    ['_'] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF },
    ['`'] = { 0x30, 0x18, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00 },
    ['a'] = { 0x00, 0x00, 0x7C, 0x06, 0x7E, 0xC6, 0x7E, 0x00 },
    ['b'] = { 0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xC6, 0xFC, 0x00 },
    ['c'] = { 0x00, 0x00, 0x7C, 0xC6, 0xC0, 0xC6, 0x7C, 0x00 },
    ['d'] = { 0x06, 0x06, 0x7E, 0xC6, 0xC6, 0xC6, 0x7E, 0x00 },
    ['e'] = { 0x00, 0x00, 0x7C, 0xC6, 0xFC, 0xC0, 0x7C, 0x00 },
    ['f'] = { 0x1C, 0x36, 0x30, 0x78, 0x30, 0x30, 0x78, 0x00 },
    ['g'] = { 0x00, 0x00, 0x7E, 0xC6, 0xC6, 0x7E, 0x06, 0x7C },
    ['h'] = { 0xC0, 0xC0, 0xFC, 0xC6, 0xC6, 0xC6, 0xC6, 0x00 },
    ['i'] = { 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00 },
    ['j'] = { 0x0C, 0x00, 0x1C, 0x0C, 0x0C, 0xCC, 0x78, 0x00 },
    ['k'] = { 0xC0, 0xC0, 0xCC, 0xD8, 0xF0, 0xD8, 0xCC, 0x00 },
    ['l'] = { 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 },
    ['m'] = { 0x00, 0x00, 0xEC, 0xFE, 0xD6, 0xD6, 0xC6, 0x00 },
    ['n'] = { 0x00, 0x00, 0xDC, 0x66, 0x66, 0x66, 0x66, 0x00 },
    ['o'] = { 0x00, 0x00, 0x7C, 0xC6, 0xC6, 0xC6, 0x7C, 0x00 },
    ['p'] = { 0x00, 0x00, 0xFC, 0xC6, 0xC6, 0xFC, 0xC0, 0xC0 },
    ['q'] = { 0x00, 0x00, 0x7E, 0xC6, 0xC6, 0x7E, 0x06, 0x06 },
    ['r'] = { 0x00, 0x00, 0xDC, 0x76, 0x60, 0x60, 0xF0, 0x00 },
    ['s'] = { 0x00, 0x00, 0x7E, 0xC0, 0x7C, 0x06, 0xFC, 0x00 },
    ['t'] = { 0x30, 0x30, 0xFC, 0x30, 0x30, 0x36, 0x1C, 0x00 },
    ['u'] = { 0x00, 0x00, 0xC6, 0xC6, 0xC6, 0xC6, 0x7E, 0x00 },
    ['v'] = { 0x00, 0x00, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00 },
    ['w'] = { 0x00, 0x00, 0xC6, 0xD6, 0xD6, 0xFE, 0x6C, 0x00 },
    ['x'] = { 0x00, 0x00, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0x00 },
    ['y'] = { 0x00, 0x00, 0xC6, 0xC6, 0xC6, 0x7E, 0x06, 0x7C },
    ['z'] = { 0x00, 0x00, 0xFE, 0x8C, 0x18, 0x32, 0xFE, 0x00 },
    ['{'] = { 0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0x00 },
    ['|'] = { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00 },
    ['}'] = { 0x70, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x70, 0x00 },
    ['~'] = { 0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

static void tty_new_line(tty_t *tty) {
    tty->col = 0;

    if (tty->row + 1 >= tty->max_rows) {
        uint32_t stride = tty_fb_stride();
        uint32_t *fb = tty_fb(tty);
        uint32_t row_bytes = stride * GLYPH_H;

        uint32_t *dst = &fb[tty->origin_y * stride];
        uint32_t *src = &fb[(tty->origin_y + GLYPH_H) * stride];
        uint32_t rows_to_move = (tty->max_rows - 1) * GLYPH_H;

        memmove(dst, src, rows_to_move * stride * sizeof(uint32_t));

        uint32_t *last_row = &fb[(tty->origin_y + (tty->max_rows - 1) * GLYPH_H) * stride];
        memset(last_row, 0, row_bytes * sizeof(uint32_t));

        tty->row = tty->max_rows - 1;
        tty_blit(tty);
    } else {
        tty->row++;
    }
}

static void tty_scroll_if_needed(tty_t *tty) {
    if (tty->row >= tty->max_rows) {
        tty_new_line(tty);
    }
}

void putchar(tty_t *tty, char c) {
    if (c == ' ') {
        tty_scroll_if_needed(tty);
        uint32_t stride = tty_fb_stride();
        uint32_t *fb = tty_fb(tty);
        uint16_t cell_w = GLYPH_W + LETTER_SPACING_PX;
        uint32_t origin_x = tty->origin_x + (tty->col * cell_w);
        uint32_t origin_y = tty->origin_y + (tty->row * GLYPH_H);

        for (uint8_t i = 0; i < GLYPH_H; i++) {
            uint32_t *row = &fb[(origin_y + i) * stride + origin_x];
            for (uint8_t j = 0; j < cell_w; j++) {
                row[j] = tty->bg;
            }
        }

        tty->col++;
    if (tty->col >= tty->max_cols)
            tty_new_line(tty);
        tty_blit_region(tty, tty->row, tty->row);
        return;
    } else if (c == '\n') {
        tty_new_line(tty);
        return;
    } else if (c == '\r') {
        tty->col = 0;
        return;
    } else if (c == '\b') {
        if (tty->col > 0) {
            tty->col--;
        } else if (tty->row > 0) {
            tty->row--;
            tty->col = tty->max_cols - 1;
        }
        tty_fill_cell(tty, tty->row, tty->col, tty->bg);
        tty_blit_region(tty, tty->row, tty->row);
        return;
    }

    uint8_t ch = (uint8_t)c;
    if (ch >= 128) return;

    const uint8_t *glyph_rows = tty_font[ch];

    tty_scroll_if_needed(tty);

    uint32_t stride = tty_fb_stride();
    uint32_t *fb = tty_fb(tty);
    uint16_t cell_w = GLYPH_W + LETTER_SPACING_PX;
    uint32_t origin_x = tty->origin_x + (tty->col * cell_w);
    uint32_t origin_y = tty->origin_y + (tty->row * GLYPH_H);

    for (uint8_t i = 0; i < GLYPH_H; i++) {
        uint8_t row_bits = glyph_rows[i];
        uint32_t *row = &fb[(origin_y + i) * stride + origin_x];
        for (uint8_t j = 0; j < GLYPH_W; j++) {
            row[j] = (row_bits & (1 << (7 - j))) ? tty->fg : tty->bg;
        }
        for (uint8_t j = 0; j < LETTER_SPACING_PX; j++) {
            row[GLYPH_W + j] = tty->bg;
        }
    }

    tty->col++;
    if (tty->col >= tty->max_cols)
        tty_new_line(tty);
    tty_blit_region(tty, tty->row, tty->row);
}

static void tty_fill_cell(tty_t *tty, uint32_t row, uint32_t col, uint32_t color) {
    if (row >= tty->max_rows || col >= tty->max_cols) return;

    uint32_t stride = tty_fb_stride();
    uint32_t *fb = tty_fb(tty);
    uint16_t cell_w = GLYPH_W + LETTER_SPACING_PX;
    uint32_t origin_x = tty->origin_x + (col * cell_w);
    uint32_t origin_y = tty->origin_y + (row * GLYPH_H);

    for (uint8_t i = 0; i < GLYPH_H; i++) {
        uint32_t *px = &fb[(origin_y + i) * stride + origin_x];
        for (uint8_t j = 0; j < cell_w; j++) {
            px[j] = color;
        }
    }
}

static void tty_clear_screen(tty_t *tty) {
    for (uint32_t r = 0; r < tty->max_rows; r++)
        for (uint32_t c = 0; c < tty->max_cols; c++)
            tty_fill_cell(tty, r, c, tty->bg);
}

static void tty_clear_line(tty_t *tty, int from, int to) {
    if (to < from) {
        int t = from; from = to; to = t;
    }
    if (from < 0) from = 0;
    if (to >= (int)tty->max_cols) to = (int)tty->max_cols - 1;
    for (int c = from; c <= to; c++)
        tty_fill_cell(tty, tty->row, (uint32_t)c, tty->bg);
}

static void tty_blit_region(tty_t *tty, uint32_t start_row, uint32_t end_row) {
    if (!tty->backbuf) return;
    uint32_t stride = tty_fb_stride();
    uint32_t *real_fb = (uint32_t *)framebuffer_request.response->framebuffers[0]->address;
    uint32_t start_y = tty->origin_y + start_row * GLYPH_H;
    uint32_t end_y = tty->origin_y + (end_row + 1) * GLYPH_H;
    if (end_y > framebuffer_request.response->framebuffers[0]->height)
        end_y = framebuffer_request.response->framebuffers[0]->height;
    uint32_t num_rows = end_y - start_y;
    memcpy(&real_fb[start_y * stride], &tty->backbuf[start_y * stride],
           num_rows * stride * sizeof(uint32_t));
}

static void tty_blit(tty_t *tty) {
    tty_blit_region(tty, 0, tty->max_rows - 1);
}

static void tty_set_cursor(tty_t *tty, uint32_t row, uint32_t col) {
    tty->row = (row >= tty->max_rows) ? tty->max_rows - 1 : row;
    tty->col = (col >= tty->max_cols) ? tty->max_cols - 1 : col;
}

static void tty_move_cursor(tty_t *tty, int drow, int dcol) {
    if (drow < 0) {
        int n = -drow;
        tty->row = (tty->row > (uint32_t)n) ? tty->row - n : 0;
    } else {
        tty->row += (uint32_t)drow;
        if (tty->row >= tty->max_rows) tty->row = tty->max_rows - 1;
    }
    if (dcol < 0) {
        int n = -dcol;
        tty->col = (tty->col > (uint32_t)n) ? tty->col - n : 0;
    } else {
        tty->col += (uint32_t)dcol;
        if (tty->col >= tty->max_cols) tty->col = tty->max_cols - 1;
    }
}

static int tty_esc_param(tty_t *tty, int i) {
    return tty->esc_param[i] ? tty->esc_param[i] : 1;
}

static void tty_esc_reset(tty_t *tty) {
    tty->esc_state = 0;
    tty->esc_priv = 0;
    tty->esc_nparam = 0;
    tty->esc_param[0] = 0;
    tty->esc_param[1] = 0;
    tty->esc_param[2] = 0;
    tty->esc_param[3] = 0;
}

static const uint32_t ansi_colors[8] = {
    0x00000000, /* 0: black   */
    0x00AA0000, /* 1: red     */
    0x0000AA00, /* 2: green   */
    0x00AA5500, /* 3: yellow  */
    0x000000AA, /* 4: blue    */
    0x00AA00AA, /* 5: magenta */
    0x0000AAAA, /* 6: cyan    */
    0x00AAAAAA, /* 7: white   */
};

static void tty_esc_finish(tty_t *tty, char c) {
    switch (c) {
        case 'A': tty_move_cursor(tty, -(int)tty_esc_param(tty, 0), 0); break;
        case 'B': tty_move_cursor(tty, tty_esc_param(tty, 0), 0); break;
        case 'C': tty_move_cursor(tty, 0, tty_esc_param(tty, 0)); break;
        case 'D': tty_move_cursor(tty, 0, -(int)tty_esc_param(tty, 0)); break;
        case 'H':
        case 'f': {
            int row = tty_esc_param(tty, 0) - 1;
            int col = tty->esc_nparam >= 1 ? tty_esc_param(tty, 1) - 1 : 0;
            tty_set_cursor(tty, (uint32_t)row, (uint32_t)col);
            break;
        }
        case 'J': {
            int n = tty_esc_param(tty, 0);
            if (n == 0) {
                for (uint32_t cc = tty->col; cc < tty->max_cols; cc++)
                    tty_fill_cell(tty, tty->row, cc, tty->bg);
                for (uint32_t rr = tty->row + 1; rr < tty->max_rows; rr++)
                    for (uint32_t cc = 0; cc < tty->max_cols; cc++)
                        tty_fill_cell(tty, rr, cc, tty->bg);
            } else if (n == 1) {
                for (uint32_t rr = 0; rr < tty->row; rr++)
                    for (uint32_t cc = 0; cc < tty->max_cols; cc++)
                        tty_fill_cell(tty, rr, cc, tty->bg);
                for (uint32_t cc = 0; cc <= tty->col; cc++)
                    tty_fill_cell(tty, tty->row, cc, tty->bg);
            } else {
                tty_clear_screen(tty);
                tty_set_cursor(tty, 0, 0);
            }
            break;
        }
        case 'K': {
            int n = tty->esc_nparam ? tty_esc_param(tty, 0) : 0;
            if (n == 0) tty_clear_line(tty, (int)tty->col, (int)tty->max_cols - 1);
            else if (n == 1) tty_clear_line(tty, 0, (int)tty->col);
            else tty_clear_line(tty, 0, (int)tty->max_cols - 1);
            break;
        }
        case 'm': {
            for (int i = 0; i <= tty->esc_nparam; i++) {
                int n = tty->esc_param[i];
                if (n == 0) {
                    tty->fg = 0xFFFFFFFF;
                    tty->bg = 0x00000000;
                } else if (n == 7) {
                    uint32_t tmp = tty->fg;
                    tty->fg = tty->bg;
                    tty->bg = tmp;
                } else if (n >= 30 && n <= 37) {
                    tty->fg = ansi_colors[n - 30];
                } else if (n >= 40 && n <= 47) {
                    tty->bg = ansi_colors[n - 40];
                } else if (n >= 90 && n <= 97) {
                    tty->fg = ansi_colors[n - 90];
                } else if (n >= 100 && n <= 107) {
                    tty->bg = ansi_colors[n - 100];
                }
            }
            break;
        }
        default:
            break;
    }
    tty_blit(tty);
    tty_esc_reset(tty);
}

static void tty_esc_input(tty_t *tty, char c) {
    if (tty->esc_state == 1) {
        if (c == '[') {
            tty->esc_state = 2;
            tty->esc_priv = 0;
            tty->esc_nparam = 0;
            tty->esc_param[0] = 0;
            tty->esc_param[1] = 0;
            tty->esc_param[2] = 0;
            tty->esc_param[3] = 0;
        } else {
            tty_esc_reset(tty);
        }
        return;
    }

    if (c == '?') {
        tty->esc_priv = 1;
        return;
    }

    if (c >= '0' && c <= '9') {
        int i = tty->esc_nparam > 3 ? 3 : tty->esc_nparam;
        tty->esc_param[i] = tty->esc_param[i] * 10 + (c - '0');
        return;
    }

    if (c == ';') {
        if (tty->esc_nparam < 3) tty->esc_nparam++;
        return;
    }

    if ((c >= '@' && c <= '~') || c == 'H' || c == 'f' || c == 'm') {
        if (tty->esc_priv == 0) {
            tty_esc_finish(tty, c);
        } else if ((c == 'l' || c == 'h') &&
                   tty->esc_nparam >= 0 && tty->esc_param[0] == 25) {
            tty->backbuf_mode = (c == 'l') ? 1 : 0;
            tty_esc_reset(tty);
        } else {
            tty_esc_reset(tty);
        }
    } else {
        tty_esc_reset(tty);
    }
}

static void ring_push(tty_ring_t *r, uint8_t c) {
    if (r->count >= TTY_BUF_SIZE) return;
    r->data[r->tail] = c;
    r->tail = (r->tail + 1) % TTY_BUF_SIZE;
    r->count++;
}

static int ring_pop(tty_ring_t *r, uint8_t *out) {
    if (r->count == 0) return 0;
    *out = r->data[r->head];
    r->head = (r->head + 1) % TTY_BUF_SIZE;
    r->count--;
    return 1;
}

static void tty_wake(tty_t *tty) {
    if (tty->waiter) {
        unblock(tty->waiter);
        tty->waiter = NULL;
    }
}

static void tty_putchar_raw(tty_t *tty, char c) {
    if ((tty->termios.c_oflag & OPOST) && (tty->termios.c_oflag & ONLCR) && c == '\n')
        tty->putchar(tty, '\r');
    tty->putchar(tty, c);
    if (c == '\n')
        outb(0xE9, '\r');
    outb(0xE9, (uint8_t)c);
}

void tty_input(tty_t *tty, char c) {
    if (c == '\r' || c == '\n' || c == 0x1b)
        print("TTYIN c=0x%x iflag=0x%x lflag=0x%x\n", (unsigned)c, tty->termios.c_iflag, tty->termios.c_lflag);
    if (tty->termios.c_iflag & ICRNL && c == '\r')
        c = '\n';

    if (tty->termios.c_lflag & ISIG) {
        if (c == tty->termios.c_cc[VINTR]) {
            if (tty->fg_pgrp) {
                kill(-(int)tty->fg_pgrp, SIGINT);
            }
            tty->raw.head = tty->raw.tail = tty->raw.count = 0;
            if (tty->termios.c_lflag & ECHO)
                tty_putchar_raw(tty, '\n');
            return;
        }
        if (c == tty->termios.c_cc[VQUIT]) {
            if (tty->fg_pgrp) {
                kill(-(int)tty->fg_pgrp, SIGQUIT);
            }
            tty->raw.head = tty->raw.tail = tty->raw.count = 0;
            if (tty->termios.c_lflag & ECHO)
                tty_putchar_raw(tty, '\n');
            return;
        }
        if (c == tty->termios.c_cc[VSUSP]) {
            if (tty->fg_pgrp) {
                kill(-(int)tty->fg_pgrp, SIGTSTP);
            }
            tty->raw.head = tty->raw.tail = tty->raw.count = 0;
            if (tty->termios.c_lflag & ECHO)
                tty_putchar_raw(tty, '\n');
            return;
        }
    }

    if (tty->termios.c_lflag & ICANON) {
        if (c == tty->termios.c_cc[VERASE]) {
            if (tty->raw.count > 0) {
                tty->raw.tail = (tty->raw.tail == 0 ? TTY_BUF_SIZE : tty->raw.tail) - 1;
                tty->raw.count--;
                if (tty->termios.c_lflag & ECHOE) {
                    tty->putchar(tty, '\b');
                    tty->putchar(tty, ' ');
                    tty->putchar(tty, '\b');
                }
            }
            return;
        }

        if (c == '\n') {
            if (tty->termios.c_lflag & ECHO)
                tty_putchar_raw(tty, c);
            ring_push(&tty->raw, (uint8_t)c);
            tty->eof_pending = 0;
            uint8_t byte;
            while (ring_pop(&tty->raw, &byte))
                ring_push(&tty->cooked, byte);
            tty_wake(tty);
            return;
        }

        if (c == tty->termios.c_cc[VEOF]) {
            tty->eof_pending = 1;
            uint8_t byte;
            while (ring_pop(&tty->raw, &byte))
                ring_push(&tty->cooked, byte);
            tty_wake(tty);
            return;
        }

        if (tty->termios.c_lflag & ECHO)
            tty_putchar_raw(tty, c);

        ring_push(&tty->raw, (uint8_t)c);
    } else {
        if (tty->termios.c_lflag & ECHO)
            tty_putchar_raw(tty, c);
        ring_push(&tty->cooked, (uint8_t)c);
        tty_wake(tty);
    }
}

int32_t tty_write(tty_t *tty, const uint8_t *buf, uint32_t count) {
    if (!tty || !tty->putchar) return -1;
    for (uint32_t i = 0; i < count; i++) {
        char c = (char)buf[i];
        if (tty->esc_state != 0) {
            tty_esc_input(tty, c);
        } else if (c == '\x1B') {
            tty->esc_state = 1;
        } else {
            tty_putchar_raw(tty, c);
        }
    }
    return (int32_t)count;
}

int32_t tty_read(tty_t *tty, uint8_t *buf, uint32_t count) {
    if (!tty || !buf || count == 0) return -1;

    if (current_tcb && current_tcb->parent && tty->fg_pgrp == 0)
        tty->fg_pgrp = current_tcb->parent->pgid;

    while (tty->cooked.count == 0) {
        if (tty->eof_pending) {
            tty->eof_pending = 0;
            return 0;
        }
        struct pcb *p = current_tcb ? current_tcb->parent : NULL;
        if (p && (p->sigstate.pending & ~p->sigstate.blocked)) {
            return -EINTR;
        }
        if (!current_tcb)
            return 0;
        asm volatile ("cli");
        if (tty->cooked.count > 0) {
            asm volatile ("sti");
            break;
        }
        tty->waiter = current_tcb;
        block_current();
    }

    uint32_t n = 0;
    while (n < count) {
        uint8_t c;
        if (!ring_pop(&tty->cooked, &c)) break;
        buf[n++] = c;
        if (tty->termios.c_lflag & ICANON && c == '\n') break;
    }
    return (int32_t)n;
}

tty_t *tty_get_active() {
    return &ttys[active_tty];
}

tty_t *tty_get(uint8_t index) {
    if (index >= TTY_MAX) return NULL;
    return &ttys[index];
}

void tty_switch(uint8_t index) {
    if (index >= TTY_MAX) return;
    ttys[active_tty].saved_render_target = ttys[active_tty].render_target;
    ttys[active_tty].active = 0;
    active_tty = index;
    ttys[active_tty].active = 1;
    if (ttys[active_tty].saved_render_target) {
        ttys[active_tty].render_target = ttys[active_tty].saved_render_target;
    }
    tty_blit(&ttys[active_tty]);
}

static ssize_t devfs_tty_read(devfs_dev_t *dev, void *buf, size_t count) {
    tty_t *tty = (tty_t *)dev->priv;
    return (ssize_t)tty_read(tty, (uint8_t *)buf, (uint32_t)count);
}

static ssize_t devfs_tty_write(devfs_dev_t *dev, const void *buf, size_t count) {
    tty_t *tty = (tty_t *)dev->priv;
    return (ssize_t)tty_write(tty, (const uint8_t *)buf, (uint32_t)count);
}

static ssize_t devfs_tty_alias_read(devfs_dev_t *dev, void *buf, size_t count) {
    (void)dev;
    return (ssize_t)tty_read(tty_get_active(), (uint8_t *)buf, (uint32_t)count);
}

static ssize_t devfs_tty_alias_write(devfs_dev_t *dev, const void *buf, size_t count) {
    (void)dev;
    return (ssize_t)tty_write(tty_get_active(), (const uint8_t *)buf, (uint32_t)count);
}

void tty_init(void (*output_fn)(tty_t *tty, char c)) {
    global_output_fn = output_fn;

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    uint32_t max_cols = (uint32_t)(fb->width / (GLYPH_W + LETTER_SPACING_PX));
    uint32_t max_rows = (uint32_t)(fb->height / GLYPH_H);

    uint32_t fb_stride = fb->pitch / 4;
    uint32_t buf_size = fb_stride * fb->height;

    for (uint8_t i = 0; i < TTY_MAX; i++) {
        tty_t *t = &ttys[i];
        memset(t, 0, sizeof(tty_t));

        t->index  = i;
        t->active = (i == 0);

        t->termios.c_iflag = ICRNL;
        t->termios.c_oflag = OPOST | ONLCR;
        t->termios.c_lflag = ECHO | ECHOE | ICANON | ISIG;
        t->termios.c_cc[VINTR]  = 0x03;
        t->termios.c_cc[VQUIT]  = 0x1C;
        t->termios.c_cc[VERASE] = 0x7F;
        t->termios.c_cc[VKILL]  = 0x15;
        t->termios.c_cc[VEOF]   = 0x04;
        t->termios.c_cc[VSUSP]  = 0x1A;
        t->termios.c_cc[VMIN]   = 1;

        t->putchar   = output_fn;
        t->col       = 0;
        t->row       = 0;
        t->max_cols  = max_cols;
        t->max_rows  = max_rows;
        t->origin_x  = 0;
        t->origin_y  = 0;
        t->fg        = 0xFFFFFFFF;
        t->bg        = 0x00000000;
        t->backbuf   = (uint32_t *)kmalloc(buf_size * sizeof(uint32_t));
        if (t->backbuf)
            memset(t->backbuf, 0, buf_size * sizeof(uint32_t));
        t->render_target = t->backbuf;
        t->saved_render_target = NULL;
        t->backbuf_mode = 0;
        t->fg_pgrp = 0;

        char name[8];
        name[0] = 't'; name[1] = 't'; name[2] = 'y';
        name[3] = '0' + i; name[4] = '\0';

        devfs_register(name, devfs_tty_read, devfs_tty_write, t);
    }

    devfs_register("tty", devfs_tty_alias_read, devfs_tty_alias_write, NULL);
}

static void tty_cc_kernel_to_user(const uint8_t *k, uint8_t *u) {
    for (int i = 0; i < 32; i++) u[i] = 0;
    u[0] = k[0];
    u[1] = k[1];
    u[2] = k[2];
    u[3] = k[3];
    u[4] = k[4];
    u[5] = k[6];
    u[6] = k[5];
    u[7] = k[7];
    u[10] = k[10];
}

static void tty_cc_user_to_kernel(const uint8_t *u, uint8_t *k) {
    for (int i = 0; i < 8; i++) k[i] = 0;
    k[0] = u[0];
    k[1] = u[1];
    k[2] = u[2];
    k[3] = u[3];
    k[4] = u[4];
    k[5] = u[6];
    k[6] = u[5];
    k[7] = u[7];
    k[10] = u[10];
    k[8] = 0;
    k[9] = 0;
}

int tty_ioctl(devfs_dev_t *dev, unsigned long req, void *arg) {
    (void)dev;
    tty_t *tty = tty_get_active();
    if (!tty || !arg) {
        return -1;
    }
    switch (req) {
        case TIOCGPGRP:
            *(int *)arg = (int)tty->fg_pgrp;
            return 0;
        case TIOCSPGRP: {
            int pgrp = *(int *)arg;
            tty->fg_pgrp = (uint64_t)pgrp;
            return 0;
        }
        default:
            return -1;
    }
}

int tty_getattr(tty_t *tty, struct termios_user *u) {
    if (!tty || !u) return -1;

    u->c_iflag = tty->termios.c_iflag;
    u->c_oflag = tty->termios.c_oflag;
    u->c_cflag = 0000060 | 0000200;
    u->c_lflag = tty->termios.c_lflag;
    u->c_line = 0;
    tty_cc_kernel_to_user(tty->termios.c_cc, u->c_cc);
    u->c_ibaud = 0000015;
    u->c_obaud = 0000015;
    return 0;
}

int tty_setattr(tty_t *tty, const struct termios_user *u) {
    if (!tty || !u) return -1;

    print("TTYSET iflag=0x%x lflag=0x%x\n", u->c_iflag, u->c_lflag);
    tty->termios.c_iflag = u->c_iflag;
    tty->termios.c_oflag = u->c_oflag;
    tty->termios.c_lflag = u->c_lflag;
    tty_cc_user_to_kernel(u->c_cc, tty->termios.c_cc);

    tty->raw.head = 0;
    tty->raw.tail = 0;
    tty->raw.count = 0;
    tty->cooked.head = 0;
    tty->cooked.tail = 0;
    tty->cooked.count = 0;
    return 0;
}

int tty_getinfo(tty_t *tty, struct ttyinfo *info) {
    if (!tty || !info) return -1;
    info->rows = tty->max_rows;
    info->cols = tty->max_cols;
    return 0;
}