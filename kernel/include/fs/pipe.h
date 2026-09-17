#pragma once

#include <stdint.h>
#include <stddef.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

#define PIPE_BUF_SIZE 4096

typedef struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    int      readers;
    int      writers;
    struct tcb *read_waiter;
    struct tcb *write_waiter;
} pipe_t;

pipe_t *pipe_create(void);
void pipe_destroy(pipe_t *p);
ssize_t pipe_read(pipe_t *p, void *buf, size_t count);
ssize_t pipe_write(pipe_t *p, const void *buf, size_t count);
void pipe_wake_readers(pipe_t *p);
void pipe_wake_writers(pipe_t *p);
