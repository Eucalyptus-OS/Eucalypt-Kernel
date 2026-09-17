#include <stdint.h>
#include <portio.h>
#include <apic.h>
#include <logging/print.h>
#include <input/keyboard.h>

#define KB_DATA_PORT   0x60
#define KB_CMD_PORT    0x64

#define KB_STATUS_OUTPUT_FULL 0x01
#define KB_STATUS_INPUT_FULL  0x02

#define KB_CMD_READ_CONFIG   0x20
#define KB_CMD_WRITE_CONFIG  0x60
#define KB_CMD_SELF_TEST     0xAA
#define KB_CMD_TEST_PORT1    0xAB
#define KB_CMD_DISABLE_PORT1 0xAD
#define KB_CMD_ENABLE_PORT1  0xAE

#define KB_DEV_SET_SCAN_SET 0xF0
#define KB_DEV_ENABLE_SCAN  0xF4
#define KB_DEV_RESET        0xFF

#define KB_ACK    0xFA
#define KB_RESEND 0xFE
#define KB_BAT_OK 0xAA

#define KB_TIMEOUT 100000

#define KB_IRQ_VECTOR 0x22

static keyboard_event_cb_t kb_event_cb;

static uint8_t kb_e0;
static uint8_t kb_mods;
static uint8_t kb_locks;

static int kb_wait_input() {
    for (int i = 0; i < KB_TIMEOUT; i++) {
        if (!(inb(KB_CMD_PORT) & KB_STATUS_INPUT_FULL))
            return 0;
    }
    return -1;
}

static int kb_wait_output() {
    for (int i = 0; i < KB_TIMEOUT; i++) {
        if (inb(KB_CMD_PORT) & KB_STATUS_OUTPUT_FULL)
            return 0;
    }
    return -1;
}

static void kb_write_cmd(uint8_t cmd) {
    while (kb_wait_input() != 0) {
    }
    outb(KB_CMD_PORT, cmd);
    io_wait();
}

static void kb_write_data(uint8_t data) {
    while (kb_wait_input() != 0) {
    }
    outb(KB_DATA_PORT, data);
    io_wait();
}

static void kb_flush() {
    while (inb(KB_CMD_PORT) & KB_STATUS_OUTPUT_FULL)
        inb(KB_DATA_PORT);
}

static uint8_t kb_await_byte() {
    if (kb_wait_output() != 0)
        return 0xFF;
    return inb(KB_DATA_PORT);
}

static int kb_send_device_cmd(uint8_t cmd) {
    for (int attempt = 0; attempt < 4; attempt++) {
        kb_write_data(cmd);
        uint8_t r = kb_await_byte();
        while (r == KB_RESEND) {
            kb_write_data(cmd);
            r = kb_await_byte();
        }
        if (r == KB_ACK)
            return 0;
    }
    return -1;
}

static void keyboard_process_scancode(uint8_t scancode) {
    if (scancode == 0xE0) {
        kb_e0 = 1;
        return;
    }
    if (scancode == 0xE1) {
        kb_e0 = 2;
        return;
    }
    if (kb_e0 == 2) {
        kb_e0 = 0;
        return;
    }

    uint8_t make = 1;
    uint8_t code = scancode & 0x7F;
    if (scancode & 0x80)
        make = 0;

    if (kb_e0) {
        switch (code) {
            case 0x1D: kb_mods = make ? (kb_mods | KB_MOD_RCTRL) : (kb_mods & ~KB_MOD_RCTRL); break;
            case 0x38: kb_mods = make ? (kb_mods | KB_MOD_RALT)  : (kb_mods & ~KB_MOD_RALT);  break;
            case 0x5B: kb_mods = make ? (kb_mods | KB_MOD_LGUI)  : (kb_mods & ~KB_MOD_LGUI);  break;
            case 0x5C: kb_mods = make ? (kb_mods | KB_MOD_RGUI)  : (kb_mods & ~KB_MOD_RGUI);  break;
            default:
                if (kb_event_cb)
                    kb_event_cb(code | KB_KEY_EXTENDED, make, kb_mods, kb_locks);
                break;
        }
        kb_e0 = 0;
        return;
    }

    switch (code) {
        case 0x1D: kb_mods = make ? (kb_mods | KB_MOD_LCTRL) : (kb_mods & ~KB_MOD_LCTRL); break;
        case 0x2A: kb_mods = make ? (kb_mods | KB_MOD_LSHIFT) : (kb_mods & ~KB_MOD_LSHIFT); break;
        case 0x36: kb_mods = make ? (kb_mods | KB_MOD_RSHIFT) : (kb_mods & ~KB_MOD_RSHIFT); break;
        case 0x38: kb_mods = make ? (kb_mods | KB_MOD_LALT) : (kb_mods & ~KB_MOD_LALT); break;
        case 0x3A: if (make) kb_locks ^= KB_LOCK_CAPS; break;
        case 0x45: if (make) kb_locks ^= KB_LOCK_NUM; break;
        case 0x46: if (make) kb_locks ^= KB_LOCK_SCROLL; break;
        default:
            if (kb_event_cb)
                kb_event_cb(code, make, kb_mods, kb_locks);
            break;
    }
}

void keyboard_set_event_cb(keyboard_event_cb_t cb) {
    kb_event_cb = cb;
}

void keyboard_handler() {
    apic_eoi();

    uint8_t scancode = inb(KB_DATA_PORT);
    keyboard_process_scancode(scancode);
}

void keyboard_init() {
    kb_e0 = 0;
    kb_mods = 0;
    kb_locks = 0;

    kb_write_cmd(KB_CMD_DISABLE_PORT1);
    kb_flush();

    kb_write_cmd(KB_CMD_SELF_TEST);
    if (kb_await_byte() != 0x55)
        print("ps2 self test failed\n");

    kb_write_cmd(KB_CMD_TEST_PORT1);
    if (kb_await_byte() != 0x00)
        print("ps2 port1 test failed\n");

    kb_write_cmd(KB_CMD_READ_CONFIG);
    uint8_t config = kb_await_byte();

    config &= ~0x40;
    config |= 0x01;
    kb_write_cmd(KB_CMD_WRITE_CONFIG);
    kb_write_data(config);

    kb_write_cmd(KB_CMD_ENABLE_PORT1);

    if (kb_send_device_cmd(KB_DEV_RESET) == 0 && kb_await_byte() != KB_BAT_OK)
        print("ps2 keyboard bat failed\n");

    if (kb_send_device_cmd(KB_DEV_SET_SCAN_SET) == 0) {
        kb_write_data(0x01);
        kb_await_byte();
    }

    kb_send_device_cmd(KB_DEV_ENABLE_SCAN);

    ioapic_write(0x12, KB_IRQ_VECTOR | ((uint64_t)(apic_read(0x20) >> 24) << 56));

    print("PS/2 keyboard initialized\n");
}
