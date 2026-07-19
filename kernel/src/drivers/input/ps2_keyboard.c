#include <portio.h>
#include <drivers/input.h>
#include <interrupts/apic.h>
#include <logging/printk.h>
#include <idt/idt.h>
#include <stdint.h>

#define PS2_KEYBOARD_IRQ     1

#define PS2_DATA_PORT        0x60
#define PS2_CONTROL_PORT     0x64
#define PS2_BUFFER_FULL      0x01
#define PS2_BUFFER_EMPTY     0x02

static const uint8_t scancode_to_keycode[] = {
    0,
    27,
    49,
    50,
    51,
    52,
    53,
    54,
    55,
    56,
    57,
    48,
    45,
    61,
    8,
    9,
    113,
    119,
    101,
    114,
    116,
    121,
    117,
    105,
    111,
    112,
    91,
    93,
    10,
    0,
    97,
    115,
    100,
    102,
    103,
    104,
    106,
    107,
    108,
    59,
    39,
    96,
    0,
    92,
    122,
    120,
    99,
    118,
    98,
    110,
    109,
    44,
    46,
    47,
    0,
    42,
    0,
    32,
    0,
};

static const uint8_t scancode_to_shifted_keycode[] = {
    0,
    27,
    33,
    64,
    35,
    36,
    37,
    94,
    38,
    42,
    40,
    41,
    95,
    43,
    8,
    9,
    81,
    87,
    69,
    82,
    84,
    89,
    85,
    73,
    79,
    80,
    123,
    125,
    10,
    0,
    65,
    83,
    68,
    70,
    71,
    72,
    74,
    75,
    76,
    58,
    34,
    126,
    0,
    124,
    90,
    88,
    67,
    86,
    66,
    78,
    77,
    60,
    62,
    63,
    0,
    42,
    0,
    32,
    0,
};

static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool capslock_active = false;
static bool extended = false;
static uint8_t keyboard_gsi = PS2_KEYBOARD_IRQ;

static void ps2_wait_write() {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_CONTROL_PORT) & 0x02)) return;
    }
}

static void ps2_wait_read() {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_CONTROL_PORT) & 0x01) return;
    }
}

static uint8_t ps2_read_data() {
    ps2_wait_read();
    return inb(PS2_DATA_PORT);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA_PORT, data);
}

static void ps2_write_command(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CONTROL_PORT, cmd);
}

void ps2_keyboard_interrupt() {
    uint8_t scancode = inb(PS2_DATA_PORT);

    if (scancode == 0xE0) {
        extended = true;
        apic_eoi();
        return;
    }

    bool key_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;

    if (code == 0x1D) {
        ctrl_pressed = !key_release;
        extended = false;
        apic_eoi();
        return;
    } else if (code == 0x2A || code == 0x36) {
        shift_pressed = !key_release;
        extended = false;
        apic_eoi();
        return;
    } else if (code == 0x38) {
        alt_pressed = !key_release;
        extended = false;
        apic_eoi();
        return;
    } else if (code == 0x3A) {
        if (!key_release) {
            capslock_active = !capslock_active;
        }
        extended = false;
        apic_eoi();
        return;
    }

    if (key_release) {
        extended = false;
        apic_eoi();
        return;
    }

    if (code < sizeof(scancode_to_keycode)) {
        uint8_t normal = scancode_to_keycode[code];
        uint8_t shifted = scancode_to_shifted_keycode[code];
        uint8_t keycode = normal;

        if (normal >= 'a' && normal <= 'z') {
            bool uppercase = shift_pressed ^ capslock_active;
            if (uppercase) {
                keycode = normal - 'a' + 'A';
            }
        } else if (shift_pressed && shifted != 0) {
            keycode = shifted;
        }

        if (keycode != 0) {
            input_event_t event = {0};
            event.timestamp = 0;
            event.type = INPUT_EVENT_KEY_PRESS;
            event.data.key.scancode = code;
            event.data.key.keycode = keycode;

            input_event_enqueue(&event);
        }
    }

    extended = false;
    apic_eoi();
}

void ps2_keyboard_init() {
    ps2_write_command(0xAD);
    ps2_write_command(0xA7);

    inb(PS2_DATA_PORT);

    ps2_write_command(0x20);
    uint8_t config = ps2_read_data();
    config |= 0x01;
    config &= ~0x02;
    config &= ~0x40;
    ps2_write_command(0x60);
    ps2_write_data(config);

    ps2_write_command(0xAE);

    ps2_write_data(0xFF);
    uint8_t resp = ps2_read_data();
    if (resp != 0xFA) {
        log_warn("PS2 keyboard reset failed: %x\n", resp);
    }

    ps2_read_data();

    ps2_write_data(0xF0);
    ps2_read_data();
    ps2_write_data(0x01);
    ps2_read_data();

    ps2_write_data(0xF4);
    ps2_read_data();

    keyboard_gsi = ioapic_route_isa_irq(PS2_KEYBOARD_IRQ, PS2_KEYBOARD_VECTOR, apic_id(), true);
    ioapic_unmask(keyboard_gsi);

    log_info("PS2 keyboard initialized\n");
}