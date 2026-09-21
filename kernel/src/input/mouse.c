#include <stdint.h>
#include <portio.h>
#include <apic.h>
#include <logging/print.h>
#include <input/evdev.h>

// PS/2 controller registers.
#define MSE_DATA_PORT 0x60
#define MSE_CMD_PORT  0x64

// IOAPIC vector and redirection entry for the mouse IRQ (pin 12 remapped).
#define MSE_IRQ_VECTOR 0x23
#define MSE_IOAPIC_ENTRY 0x28

// Busy-wait iterations before giving up on the controller.
#define MSE_TIMEOUT 100000

static uint8_t packet[4];
static uint8_t cycle;
static uint8_t has_wheel;
static uint8_t last_buttons;

// Spin until the controller can accept a byte, or until the timeout hits.
static int ms_wait_input() {
    for (int i = 0; i < MSE_TIMEOUT; i++) {
        if (!(inb(MSE_CMD_PORT) & 0x02))
            return 0;
    }
    return -1;
}

// Spin until the controller has data to read, or until the timeout hits.
static int ms_wait_output() {
    for (int i = 0; i < MSE_TIMEOUT; i++) {
        if (inb(MSE_CMD_PORT) & 0x01)
            return 0;
    }
    return -1;
}

// Write a command byte to the controller port.
static void ms_write_cmd(uint8_t cmd) {
    while (ms_wait_input() != 0) {
    }
    outb(MSE_CMD_PORT, cmd);
    io_wait();
}

// Write a data byte to the PS/2 controller data port.
static void ms_write_data(uint8_t byte) {
    while (ms_wait_input() != 0) {
    }
    outb(MSE_DATA_PORT, byte);
    io_wait();
}

// Read one byte from the controller output buffer (blades on timeout).
static uint8_t ms_read_data() {
    while (ms_wait_output() != 0) {
    }
    return inb(MSE_DATA_PORT);
}

// Send a byte addressed to the mouse device via the 0xD4 pass-through channel.
static void ms_device_cmd(uint8_t byte) {
    ms_write_cmd(0xD4);
    ms_write_data(byte);
}

// Send a command with an argument byte (both via the mouse channel), consuming
// the two ACK bytes.
static void ms_device_cmd_arg(uint8_t cmd, uint8_t arg) {
    ms_device_cmd(cmd);
    ms_read_data();
    ms_device_cmd(arg);
    ms_read_data();
}

// Decode one complete packet and publish it to /dev/event1.
static void report_packet() {
    uint8_t flags = packet[0];
    if (flags & 0xC0)                 // overflow bits set: drop this packet
        return;

    int dx = packet[1] - ((flags << 4) & 0x100);
    int dy = packet[2] - ((flags << 3) & 0x100);
    uint8_t buttons = flags & 0x07;

    evdev_emit(EVDEV_MSE, EV_REL, REL_X, dx);
    evdev_emit(EVDEV_MSE, EV_REL, REL_Y, -dy);

    if ((buttons & 1) != (last_buttons & 1))
        evdev_emit(EVDEV_MSE, EV_KEY, BTN_LEFT, (buttons & 1) ? 1 : 0);
    if ((buttons & 2) != (last_buttons & 2))
        evdev_emit(EVDEV_MSE, EV_KEY, BTN_RIGHT, (buttons & 2) ? 1 : 0);
    if ((buttons & 4) != (last_buttons & 4))
        evdev_emit(EVDEV_MSE, EV_KEY, BTN_MIDDLE, (buttons & 4) ? 1 : 0);
    last_buttons = buttons;

    if (has_wheel) {
        int wheel = (int8_t)(packet[3] << 4) >> 4;   // signed low nibble
        if (wheel != 0)
            evdev_emit(EVDEV_MSE, EV_REL, REL_WHEEL, wheel);
    }

    evdev_emit(EVDEV_MSE, EV_SYN, SYN_REPORT, 0);
}

// PS/2 mouse IRQ: capture one byte; assemble 3- or 4-byte packets and report.
void mouse_handler() {
    uint8_t status = inb(MSE_CMD_PORT);

    if ((status & 0x21) == 0x21) {            // output full + from mouse
        uint8_t data = inb(MSE_DATA_PORT);

        if (!(cycle == 0 && !(data & 0x08))) { // resync: first byte needs bit 3
            packet[cycle++] = data;

            uint8_t len = has_wheel ? 4 : 3;
            if (cycle == len) {
                cycle = 0;
                report_packet();
            }
        }
    }

    apic_eoi();
}

void mouse_init() {
    ms_write_cmd(0xA8);                       // enable PS/2 port 2

    // Read the controller config byte, enable the port 2 interrupt and clock,
    // and write it back preserving the keyboard settings.
    ms_write_cmd(0x20);
    uint8_t config = ms_read_data();
    config |= 0x02;
    config &= ~0x20;
    ms_write_cmd(0x60);
    ms_write_data(config);

    while (inb(MSE_CMD_PORT) & 0x01)          // flush any stale output
        inb(MSE_DATA_PORT);

    ms_device_cmd(0xF6);                      // set defaults
    ms_read_data();

    // imps/2 probe: 200/100/80 Hz sample rates enable wheel reporting, which
    // the device confirms by returning ID 3 on the next get-device-id.
    ms_device_cmd_arg(0xF3, 200);
    ms_device_cmd_arg(0xF3, 100);
    ms_device_cmd_arg(0xF3, 80);

    ms_device_cmd(0xF2);                      // get device id
    ms_read_data();
    uint8_t id = ms_read_data();
    has_wheel = (id == 3);
    print("PS/2 mouse device id: %X has_wheel=%d\n", id, has_wheel);

    // Route IRQ12 (I/O APIC pin 12) to our vector, edge-triggered like the
    // keyboard so the raw entry write is used rather than ioapic_set_entry.
    ioapic_write(MSE_IOAPIC_ENTRY, MSE_IRQ_VECTOR | ((uint64_t)(apic_read(0x20) >> 24) << 56));

    ms_device_cmd(0xF4);                      // enable data reporting
    ms_read_data();

    print("PS/2 mouse initialized\n");
}