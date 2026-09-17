#include <fs/pipe.h>
#include <mm/heap.h>
#include <multitasking/sched.h>
#include <multitasking/thread.h>
#include <signal.h>
#include <logging/print.h>

static void pipe_push(pipe_t *p, uint8_t c) {
    if (p->count >= PIPE_BUF_SIZE) return;
    p->buf[p->tail] = c;
    p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    p->count++;
}

static int pipe_pop(pipe_t *p, uint8_t *out) {
    if (p->count == 0) return 0;
    *out = p->buf[p->head];
    p->head = (p->head + 1) % PIPE_BUF_SIZE;
    p->count--;
    return 1;
}

pipe_t *pipe_create(void) {
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));
    if (!p) return 0;
    p->head = p->tail = p->count = 0;
    p->readers = 1;
    p->writers = 1;
    p->read_waiter = 0;
    p->write_waiter = 0;
    return p;
}

void pipe_destroy(pipe_t *p) {
    if (p) kfree(p);
}

ssize_t pipe_read(pipe_t *p, void *buf, size_t count) {
    if (!p) return -1;
    uint8_t *out = (uint8_t *)buf;
    size_t n = 0;

    while (n < count) {
        uint8_t c;
        if (!pipe_pop(p, &c)) {
            if (n > 0) return (ssize_t)n;
            if (p->writers == 0) return 0;
            p->read_waiter = current_tcb;
            block_current();
            continue;
        }
        out[n++] = c;
        if (c == '\n') break;
    }
    return (ssize_t)n;
}

ssize_t pipe_write(pipe_t *p, const void *buf, size_t count) {
    if (!p) return -1;
    const uint8_t *src = (const uint8_t *)buf;
    size_t n = 0;

    while (n < count) {
        if (p->readers == 0) {
            struct pcb *proc = sched_current_proc();
            if (proc) sig_queue(proc, SIGPIPE);
            return -1;
        }
        if (p->count >= PIPE_BUF_SIZE) {
            p->write_waiter = current_tcb;
            block_current();
            continue;
        }
        pipe_push(p, src[n]);
        n++;
        if (p->read_waiter) {
            struct tcb *t = p->read_waiter;
            p->read_waiter = 0;
            unblock(t);
        }
    }
    return (ssize_t)n;
}

void pipe_wake_readers(pipe_t *p) {
    if (p && p->read_waiter) {
        struct tcb *t = p->read_waiter;
        p->read_waiter = 0;
        unblock(t);
    }
}

void pipe_wake_writers(pipe_t *p) {
    if (p && p->write_waiter) {
        struct tcb *t = p->write_waiter;
        p->write_waiter = 0;
        unblock(t);
    }
}
