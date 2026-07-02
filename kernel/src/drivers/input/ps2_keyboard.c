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

// PS2 scancode to keycode mapping (simplified - US layout)
static const uint8_t scancode_to_keycode[] = {
    0,    // 0x00
    27,   // 0x01 ESC
    49,   // 0x02 1
    50,   // 0x03 2
    51,   // 0x04 3
    52,   // 0x05 4
    53,   // 0x06 5
    54,   // 0x07 6
    55,   // 0x08 7
    56,   // 0x09 8
    57,   // 0x0A 9
    48,   // 0x0B 0
    45,   // 0x0C -
    61,   // 0x0D =
    8,    // 0x0E BACKSPACE
    9,    // 0x0F TAB
    113,  // 0x10 Q
    119,  // 0x11 W
    101,  // 0x12 E
    114,  // 0x13 R
    116,  // 0x14 T
    121,  // 0x15 Y
    117,  // 0x16 U
    105,  // 0x17 I
    111,  // 0x18 O
    112,  // 0x19 P
    91,   // 0x1A [
    93,   // 0x1B ]
    10,   // 0x1C ENTER
    0,    // 0x1D LCTRL
    97,   // 0x1E A
    115,  // 0x1F S
    100,  // 0x20 D
    102,  // 0x21 F
    103,  // 0x22 G
    104,  // 0x23 H
    106,  // 0x24 J
    107,  // 0x25 K
    108,  // 0x26 L
    59,   // 0x27 ;
    39,   // 0x28 '
    96,   // 0x29 `
    0,    // 0x2A LSHIFT
    92,   // 0x2B backslash
    122,  // 0x2C Z
    120,  // 0x2D X
    99,   // 0x2E C
    118,  // 0x2F V
    98,   // 0x30 B
    110,  // 0x31 N
    109,  // 0x32 M
    44,   // 0x33 ,
    46,   // 0x34 .
    47,   // 0x35 /
    0,    // 0x36 RSHIFT
    42,   // 0x37 *
    0,    // 0x38 LALT
    32,   // 0x39 SPACE
    0,    // 0x3A CAPSLOCK
};

static const uint8_t scancode_to_shifted_keycode[] = {
    0,    // 0x00
    27,   // 0x01 ESC
    33,   // 0x02 !
    64,   // 0x03 @
    35,   // 0x04 #
    36,   // 0x05 $
    37,   // 0x06 %
    94,   // 0x07 ^
    38,   // 0x08 &
    42,   // 0x09 *
    40,   // 0x0A (
    41,   // 0x0B )
    95,   // 0x0C _
    43,   // 0x0D +
    8,    // 0x0E BACKSPACE
    9,    // 0x0F TAB
    81,   // 0x10 Q
    87,   // 0x11 W
    69,   // 0x12 E
    82,   // 0x13 R
    84,   // 0x14 T
    89,   // 0x15 Y
    85,   // 0x16 U
    73,   // 0x17 I
    79,   // 0x18 O
    80,   // 0x19 P
    123,  // 0x1A {
    125,  // 0x1B }
    10,   // 0x1C ENTER
    0,    // 0x1D LCTRL
    65,   // 0x1E A
    83,   // 0x1F S
    68,   // 0x20 D
    70,   // 0x21 F
    71,   // 0x22 G
    72,   // 0x23 H
    74,   // 0x24 J
    75,   // 0x25 K
    76,   // 0x26 L
    58,   // 0x27 :
    34,   // 0x28 "
    126,  // 0x29 ~
    0,    // 0x2A LSHIFT
    124,  // 0x2B |
    90,   // 0x2C Z
    88,   // 0x2D X
    67,   // 0x2E C
    86,   // 0x2F V
    66,   // 0x30 B
    78,   // 0x31 N
    77,   // 0x32 M
    60,   // 0x33 
    62,   // 0x34 >
    63,   // 0x35 ?
    0,    // 0x36 RSHIFT
    42,   // 0x37 *
    0,    // 0x38 LALT
    32,   // 0x39 SPACE
    0,    // 0x3A CAPSLOCK
};

static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool alt_pressed = false;
static bool capslock_active = false;
static bool extended = false;

static void ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_CONTROL_PORT) & 0x02)) return;
    }
}

static void ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_CONTROL_PORT) & 0x01) return;
    }
}

static uint8_t ps2_read_data(void) {
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

void ps2_keyboard_interrupt(void) {
    uint8_t scancode = inb(PS2_DATA_PORT);

    if (scancode == 0xE0) {
        extended = true;
        apic_eoi();
        return;
    }

    bool key_release = (scancode & 0x80) != 0;
    uint8_t code = scancode & 0x7F;


    if (code == 0x1D) { // CTRL
        ctrl_pressed = !key_release;
        extended = false;
        apic_eoi();
        return;
    } else if (code == 0x2A || code == 0x36) { // SHIFT
        shift_pressed = !key_release;
        extended = false;
        apic_eoi();
        return;
    } else if (code == 0x38) { // ALT
        alt_pressed = !key_release;
        extended = false;
        apic_eoi();
        return;
    } else if (code == 0x3A) { // CAPSLOCK
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

void ps2_keyboard_init(void) {
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

    ioapic_set_entry(PS2_KEYBOARD_IRQ, PS2_KEYBOARD_VECTOR, apic_id(), true);
    ioapic_unmask(PS2_KEYBOARD_IRQ);

    log_info("PS2 keyboard initialized\n");
}