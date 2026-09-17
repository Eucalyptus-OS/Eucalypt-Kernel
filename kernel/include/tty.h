#pragma once

#include <stdint.h>

#define ICRNL   0x0100
#define IXON    0x0400
#define OPOST   0x0001
#define ONLCR   0x0004
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHONL  0x0040
#define ICANON  0x0002
#define ISIG    0x0001

#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VMIN    5
#define VTIME   6
#define VSUSP   10
#define NCCS    11

#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_lflag;
    uint8_t  c_cc[NCCS];
} termios_t;

#define TTY_BUF_SIZE 4096

typedef struct {
    uint8_t  data[TTY_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} tty_ring_t;

#define TTY_MAX 4

typedef struct tty tty_t;

struct tcb;

struct tty {
    uint8_t    index;
    termios_t  termios;
    tty_ring_t raw;
    tty_ring_t cooked;
    void (*putchar)(tty_t *tty, char c);
    void (*push_char)(tty_t *tty, char c);
    uint8_t    active;
    struct tcb *waiter;

    uint32_t   col;
    uint32_t   row;
    uint32_t   max_cols;
    uint32_t   max_rows;
    uint32_t   origin_x;
    uint32_t   origin_y;

    uint8_t    esc_state;
    uint8_t    esc_priv;
    int        esc_param[4];
    int        esc_nparam;
    uint32_t   fg;
    uint32_t   bg;
    uint32_t  *backbuf;
    uint32_t  *render_target;
    uint32_t  *saved_render_target;
    int        backbuf_mode;
    uint64_t   fg_pgrp;
};

struct termios_user {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[32];
    uint32_t c_ibaud;
    uint32_t c_obaud;
};

struct ttyinfo {
    uint32_t rows;
    uint32_t cols;
};

void tty_init(void (*output_fn)(tty_t *tty, char c));
void tty_switch(uint8_t index);
tty_t *tty_get_active();
tty_t *tty_get(uint8_t index);
void tty_input(tty_t *tty, char c);
int32_t tty_write(tty_t *tty, const uint8_t *buf, uint32_t count);
int32_t tty_read(tty_t *tty, uint8_t *buf, uint32_t count);
int tty_getattr(tty_t *tty, struct termios_user *u);
int tty_setattr(tty_t *tty, const struct termios_user *u);
int tty_getinfo(tty_t *tty, struct ttyinfo *info);