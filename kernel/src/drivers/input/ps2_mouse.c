#include <portio.h>
#include <drivers/input.h>
#include <interrupts/apic.h>
#include <logging/printk.h>
#include <idt/idt.h>
#include <stdint.h>

#define PS2_MOUSE_IRQ        12

#define PS2_DATA_PORT        0x60
#define PS2_CONTROL_PORT     0x64

#define MOUSE_LEFT_BUTTON    0x01
#define MOUSE_RIGHT_BUTTON   0x02
#define MOUSE_MIDDLE_BUTTON  0x04
#define MOUSE_X_SIGN         0x10
#define MOUSE_Y_SIGN         0x20
#define MOUSE_X_OVERFLOW     0x40
#define MOUSE_Y_OVERFLOW     0x80

static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static uint8_t mouse_buttons = 0;

static uint8_t mouse_packet[3];
static uint8_t mouse_packet_idx = 0;

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

void ps2_mouse_interrupt(void) {
    uint8_t status = inb(PS2_CONTROL_PORT);
    
    // Check if the byte is from the mouse
    if (!(status & 0x20)) {
        apic_eoi();
        return;
    }
    
    mouse_packet[mouse_packet_idx] = inb(PS2_DATA_PORT);
    
    // First byte should have bit 3 set
    if (mouse_packet_idx == 0 && !(mouse_packet[0] & 0x08)) {
        apic_eoi();
        return;
    }
    
    mouse_packet_idx++;
    
    if (mouse_packet_idx == 3) {
        mouse_packet_idx = 0;
        
        // Process complete packet
        uint8_t byte0 = mouse_packet[0];
        int8_t dx = mouse_packet[1];
        int8_t dy = mouse_packet[2];
        
        // Handle sign extension for overflow
        if (byte0 & MOUSE_X_OVERFLOW) dx = 0;
        if (byte0 & MOUSE_Y_OVERFLOW) dy = 0;
        
        // Y delta is inverted in PS/2
        dy = -dy;
        
        // Update mouse position
        mouse_x += dx;
        mouse_y += dy;
        
        // Clamp to reasonable screen bounds
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x > 4096) mouse_x = 4096;
        if (mouse_y > 2048) mouse_y = 2048;
        
        // Detect button changes
        uint8_t new_buttons = 0;
        if (byte0 & MOUSE_LEFT_BUTTON) new_buttons |= MOUSE_BUTTON_LEFT;
        if (byte0 & MOUSE_RIGHT_BUTTON) new_buttons |= MOUSE_BUTTON_RIGHT;
        if (byte0 & MOUSE_MIDDLE_BUTTON) new_buttons |= MOUSE_BUTTON_MIDDLE;
        
        uint8_t button_changed = mouse_buttons ^ new_buttons;
        
        // Generate motion event
        if (dx != 0 || dy != 0) {
            input_event_t event = {0};
            event.type = INPUT_EVENT_MOUSE_MOTION;
            event.timestamp = 0;
            event.data.mouse.dx = dx;
            event.data.mouse.dy = dy;
            event.data.mouse.buttons = new_buttons;
            input_event_enqueue(&event);
        }
        
        // Generate button events
        if (button_changed) {
            if ((button_changed & MOUSE_BUTTON_LEFT) && (new_buttons & MOUSE_BUTTON_LEFT)) {
                input_event_t event = {0};
                event.type = INPUT_EVENT_MOUSE_BUTTON_PRESS;
                event.timestamp = 0;
                event.data.mouse.buttons = MOUSE_BUTTON_LEFT;
                input_event_enqueue(&event);
            }
            if ((button_changed & MOUSE_BUTTON_LEFT) && !(new_buttons & MOUSE_BUTTON_LEFT)) {
                input_event_t event = {0};
                event.type = INPUT_EVENT_MOUSE_BUTTON_RELEASE;
                event.timestamp = 0;
                event.data.mouse.buttons = MOUSE_BUTTON_LEFT;
                input_event_enqueue(&event);
            }
            
            if ((button_changed & MOUSE_BUTTON_RIGHT) && (new_buttons & MOUSE_BUTTON_RIGHT)) {
                input_event_t event = {0};
                event.type = INPUT_EVENT_MOUSE_BUTTON_PRESS;
                event.timestamp = 0;
                event.data.mouse.buttons = MOUSE_BUTTON_RIGHT;
                input_event_enqueue(&event);
            }
            if ((button_changed & MOUSE_BUTTON_RIGHT) && !(new_buttons & MOUSE_BUTTON_RIGHT)) {
                input_event_t event = {0};
                event.type = INPUT_EVENT_MOUSE_BUTTON_RELEASE;
                event.timestamp = 0;
                event.data.mouse.buttons = MOUSE_BUTTON_RIGHT;
                input_event_enqueue(&event);
            }
            
            if ((button_changed & MOUSE_BUTTON_MIDDLE) && (new_buttons & MOUSE_BUTTON_MIDDLE)) {
                input_event_t event = {0};
                event.type = INPUT_EVENT_MOUSE_BUTTON_PRESS;
                event.timestamp = 0;
                event.data.mouse.buttons = MOUSE_BUTTON_MIDDLE;
                input_event_enqueue(&event);
            }
            if ((button_changed & MOUSE_BUTTON_MIDDLE) && !(new_buttons & MOUSE_BUTTON_MIDDLE)) {
                input_event_t event = {0};
                event.type = INPUT_EVENT_MOUSE_BUTTON_RELEASE;
                event.timestamp = 0;
                event.data.mouse.buttons = MOUSE_BUTTON_MIDDLE;
                input_event_enqueue(&event);
            }
        }
        
        mouse_buttons = new_buttons;
    }
    
    apic_eoi();
}

void ps2_mouse_init(void) {
    // Enable auxiliary port (mouse)
    ps2_write_command(0xA8);
    
    // Set controller configuration byte to enable mouse interrupt
    ps2_write_command(0x20);
    uint8_t config = ps2_read_data();
    config |= 0x02;  // Enable mouse interrupt
    config &= ~0x20; // Disable PS/2 clock
    ps2_write_command(0x60);
    ps2_write_data(config);
    
    // Use default configuration for mouse
    ps2_write_command(0xD4);
    ps2_write_data(0xF6); // Default settings
    ps2_read_data(); // ACK
    
    // Enable data reporting
    ps2_write_command(0xD4);
    ps2_write_data(0xF4);
    ps2_read_data(); // ACK

    ioapic_set_entry(PS2_MOUSE_IRQ, PS2_MOUSE_VECTOR, apic_id(), true);
    ioapic_unmask(PS2_MOUSE_IRQ);
    
    log_info("PS2 mouse initialized\n");
}
