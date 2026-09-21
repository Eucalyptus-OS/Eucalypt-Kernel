#include <stdint.h>
#include <stddef.h>
#include <mm/memory.h>
#include <input/keyboard.h>
#include <fs/devfs.h>
#include <multitasking/sched.h>
#include <multitasking/thread.h>
#include <abi/errno.h>
#include <input/evdev.h>

// Number of input_event records each device ring can hold.
#define EVDEV_RING_SIZE 64

// The evdev devices this kernel exposes: event0 (keyboard) and event1 (mouse).
#define EVDEV_NUM_DEVICES 2

// evdev ioctl request codes, encoded with the Linux _IOR() layout (type 'E').
#define EVIOCGVERSION 0x80044501u
#define EVIOCGID      0x80084502u
// EVIOCGNAME(len) is _IOR('E', 0x06, len): the low 16 bits always hold
// type|nr regardless of len, while (req >> 16) & 0x3FFF carries the size.
#define EVIOCGNAME_TAG  0x4506u
#define EVIOCGNAME_MASK 0xFFFFu

// Linux input_id fields returned by EVIOCGID.
struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

#define BUS_I8042      0x08
#define EVDEV_VERSION  0x010001

// Ring + read state shared between the ISR (producer) and read() for one device.
struct evdev_dev {
    const char *name;        // device name returned by EVIOCGNAME
    uint16_t    product;     // EVIOCGID product id
    struct input_event ring[EVDEV_RING_SIZE];
    uint32_t head;           // oldest event
    uint32_t tail;           // next write slot
    uint32_t count;
    struct tcb *waiter;      // thread blocked in this device's read
};

static struct evdev_dev g_devices[EVDEV_NUM_DEVICES] = {
    { .name = "Eucalypt PS/2 Keyboard", .product = 0x0001 },
    { .name = "Eucalypt PS/2 Mouse",    .product = 0x0002 },
};

// Keyboard auto-repeat detection state (only meaningful for EVDEV_KBD).
static uint8_t g_prev_ext;
static uint8_t g_prev_code;
static uint8_t g_prev_down;

static uint64_t evdev_rdtsc() {
    uint32_t lo, hi;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Translate a keyboard.c key (set-1 scancode, bit 7 = E0-extended) to a Linux
// KEY_* code. Set-1 scancodes already equal their KEY_* values for nearly every
// key, so only the numpad asterisk and the E0-extended block need a table.
static uint16_t evdev_scancode_to_key(uint8_t key) {
    if (key & KB_KEY_EXTENDED) {
        switch (key & 0x7F) {
            case 0x1C: return 96;  // KEY_KPENTER
            case 0x1D: return 97;  // KEY_RIGHTCTRL
            case 0x35: return 98;  // KEY_KPSLASH
            case 0x38: return 100; // KEY_RIGHTALT
            case 0x47: return 102; // KEY_HOME
            case 0x48: return 103; // KEY_UP
            case 0x49: return 104; // KEY_PAGEUP
            case 0x4B: return 105; // KEY_LEFT
            case 0x4D: return 106; // KEY_RIGHT
            case 0x4F: return 107; // KEY_END
            case 0x50: return 108; // KEY_DOWN
            case 0x51: return 109; // KEY_PAGEDOWN
            case 0x52: return 110; // KEY_INSERT
            case 0x53: return 111; // KEY_DELETE
            case 0x5B: return 125; // KEY_LEFTMETA
            case 0x5C: return 126; // KEY_RIGHTMETA
            case 0x5D: return 127; // KEY_COMPOSE (menu key)
            default:   return 0;
        }
    }
    if (key == 0x37)
        return 63;                 // KEY_KPASTERISK (numpad *)
    if (key < 128)
        return key;                // every other set-1 code is its own KEY_*
    return 0;
}

// Stamp one event with the current time and publish it to the device ring,
// waking any blocked reader. Runs in an IRQ handler.
void evdev_emit(uint32_t dev, uint16_t type, uint16_t code, int32_t value) {
    if (dev >= EVDEV_NUM_DEVICES)
        return;
    struct evdev_dev *d = &g_devices[dev];

    struct input_event ev;
    uint64_t tsc = evdev_rdtsc();
    ev.tv_sec  = (int64_t)(tsc / 1000000000ULL);
    ev.tv_usec = (int64_t)((tsc % 1000000000ULL) / 1000ULL);
    ev.type  = type;
    ev.code  = code;
    ev.value = value;

    if (d->count < EVDEV_RING_SIZE) {
        d->ring[d->tail] = ev;
        d->tail = (d->tail + 1) % EVDEV_RING_SIZE;
        d->count++;
    }

    if (d->waiter) {
        unblock(d->waiter);
        d->waiter = NULL;
    }
}

// Keyboard ISR hook: emit an EV_MSC + EV_KEY + EV_SYN packet per event.
void evdev_report(uint8_t key, uint8_t value) {
    uint16_t code = evdev_scancode_to_key(key);
    if (code == 0)
        return;

    // PS/2 auto-repeat resends the make code with no break in between; detect
    // that and report it as a repeat (value 2) like Linux does.
    uint8_t ext  = (key & KB_KEY_EXTENDED) ? 1 : 0;
    uint8_t base = key & 0x7F;
    if (value == 1 && g_prev_down &&
        g_prev_ext == ext && g_prev_code == base)
        value = 2;

    if (value == 1) {
        g_prev_down = 1;
        g_prev_ext  = ext;
        g_prev_code = base;
    } else if (value == 0) {
        g_prev_down = 0;
    }

    evdev_emit(EVDEV_KBD, EV_MSC, MSC_SCAN, key);
    evdev_emit(EVDEV_KBD, EV_KEY, code, value);
    evdev_emit(EVDEV_KBD, EV_SYN, SYN_REPORT, 0);
}

// Pop queued events into the caller's buffer, blocking until at least one is
// available. Only whole 24-byte events are delivered.
static ssize_t evdev_read(devfs_dev_t *dev, void *buf, size_t count) {
    struct evdev_dev *d = (struct evdev_dev *)dev->priv;
    if (!buf || count == 0)
        return -1;

    while (d->count == 0) {
        struct pcb *p = current_tcb ? current_tcb->parent : NULL;
        if (p && (p->sigstate.pending & ~p->sigstate.blocked))
            return -EINTR;
        if (!current_tcb)
            return 0;
        asm volatile ("cli");
        if (d->count > 0) {
            asm volatile ("sti");
            break;
        }
        d->waiter = current_tcb;
        block_current();
    }

    uint8_t *out = (uint8_t *)buf;
    size_t n = 0;
    while (n + sizeof(struct input_event) <= count) {
        if (d->count == 0)
            break;
        memcpy(out + n, &d->ring[d->head], sizeof(struct input_event));
        d->head = (d->head + 1) % EVDEV_RING_SIZE;
        d->count--;
        n += sizeof(struct input_event);
    }
    return (ssize_t)n;
}

static int evdev_ioctl(devfs_dev_t *dev, unsigned long req, void *arg) {
    struct evdev_dev *d = (struct evdev_dev *)dev->priv;
    if (!arg)
        return -1;

    switch (req) {
        case EVIOCGVERSION:
            *(int *)arg = EVDEV_VERSION;
            return 0;
        case EVIOCGID: {
            struct input_id id;
            id.bustype = BUS_I8042;
            id.vendor  = 0x0001;
            id.product = d->product;
            id.version = 0x0100;
            memcpy(arg, &id, sizeof(id));
            return 0;
        }
        default:
            break;
    }

    if (((unsigned long)req & EVIOCGNAME_MASK) == EVIOCGNAME_TAG) {
        size_t avail = (size_t)((req >> 16) & 0x3FFF);
        if (avail == 0)
            return 0;
        size_t len = 0;
        while (d->name[len] != 0)
            len++;
        size_t n = len + 1;          // include the terminating NUL
        if (n > avail)
            n = avail;
        memcpy(arg, d->name, n);
        ((char *)arg)[n - 1] = 0;
        return (int)n;
    }
    return -1;
}

void evdev_init() {
    static const char *devnames[EVDEV_NUM_DEVICES] = { "event0", "event1" };
    for (int i = 0; i < EVDEV_NUM_DEVICES; i++) {
        if (devfs_register(devnames[i], evdev_read, NULL, &g_devices[i]) != 0)
            continue;
        devfs_dev_t *dev = devfs_get(devnames[i]);
        if (dev)
            dev->ioctl = evdev_ioctl;
    }
}