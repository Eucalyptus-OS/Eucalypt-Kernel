#include <stdint.h>
#include <input/input.h>
#include <input/keyboard.h>
#include <input/mouse.h>
#include <input/evdev.h>
#include <tty.h>

// Ring-buffer capacity for the global input queue.
#define INPUT_BUF_SIZE 256

// Key codes handled specially before the plain character path.
#define INPUT_KEY_ESC        0x01
#define INPUT_KEY_BACKSPACE  0x0E
#define INPUT_KEY_TAB        0x0F
#define INPUT_KEY_ENTER      0x1C

// Global keyboard input ring buffer (head = oldest, tail = next write).
static uint8_t  input_buf[INPUT_BUF_SIZE];
static uint32_t input_head;
static uint32_t input_tail;
static uint32_t input_count;

// US layout: scancode -> [unshifted, shifted] characters.
static const char input_keymap[128][2] = {
    [0x02] = { '1', '!' },
    [0x03] = { '2', '@' },
    [0x04] = { '3', '#' },
    [0x05] = { '4', '$' },
    [0x06] = { '5', '%' },
    [0x07] = { '6', '^' },
    [0x08] = { '7', '&' },
    [0x09] = { '8', '*' },
    [0x0A] = { '9', '(' },
    [0x0B] = { '0', ')' },
    [0x0C] = { '-', '_' },
    [0x0D] = { '=', '+' },
    [0x10] = { 'q', 'Q' },
    [0x11] = { 'w', 'W' },
    [0x12] = { 'e', 'E' },
    [0x13] = { 'r', 'R' },
    [0x14] = { 't', 'T' },
    [0x15] = { 'y', 'Y' },
    [0x16] = { 'u', 'U' },
    [0x17] = { 'i', 'I' },
    [0x18] = { 'o', 'O' },
    [0x19] = { 'p', 'P' },
    [0x1A] = { '[', '{' },
    [0x1B] = { ']', '}' },
    [0x1E] = { 'a', 'A' },
    [0x1F] = { 's', 'S' },
    [0x20] = { 'd', 'D' },
    [0x21] = { 'f', 'F' },
    [0x22] = { 'g', 'G' },
    [0x23] = { 'h', 'H' },
    [0x24] = { 'j', 'J' },
    [0x25] = { 'k', 'K' },
    [0x26] = { 'l', 'L' },
    [0x27] = { ';', ':' },
    [0x28] = { '\'', '"' },
    [0x29] = { '`', '~' },
    [0x2B] = { '\\', '|' },
    [0x2C] = { 'z', 'Z' },
    [0x2D] = { 'x', 'X' },
    [0x2E] = { 'c', 'C' },
    [0x2F] = { 'v', 'V' },
    [0x30] = { 'b', 'B' },
    [0x31] = { 'n', 'N' },
    [0x32] = { 'm', 'M' },
    [0x33] = { ',', '<' },
    [0x34] = { '.', '>' },
    [0x35] = { '/', '?' },
    [0x37] = { '*', '*' },
    [0x39] = { ' ', ' ' },
};

// Queue a character locally and forward it to the active TTY.
static void input_push(char c) {
    if (input_count < INPUT_BUF_SIZE) {
        input_buf[input_tail] = (uint8_t)c;
        input_tail = (input_tail + 1) % INPUT_BUF_SIZE;
        input_count++;
    }

    tty_t *tty = tty_get_active();
    if (tty)
        tty_input(tty, c);
}

// Translate one keyboard event (key code + modifiers) into characters.
void input_handle_event(uint8_t key, uint8_t make, uint8_t mods, uint8_t locks) {
    if (!make)
        return;

    uint8_t ctrl  = (mods & (KB_MOD_LCTRL | KB_MOD_RCTRL)) != 0;
    uint8_t alt   = (mods & (KB_MOD_LALT | KB_MOD_RALT)) != 0;

    // Ctrl+Alt+F1..F4 switches to virtual console 0..3.
    if (ctrl && alt && !(key & KB_KEY_EXTENDED) && key >= 0x3B && key <= 0x3E) {
        tty_switch((uint8_t)(key - 0x3B));
        return;
    }

    if (key == INPUT_KEY_ENTER) {
        input_push('\r');
        return;
    }
    if (key == INPUT_KEY_BACKSPACE) {
        input_push('\x7F');
        return;
    }
    if (key == INPUT_KEY_TAB) {
        input_push('\t');
        return;
    }
    if (key == INPUT_KEY_ESC) {
        input_push('\x1B');
        return;
    }

    // Extended keys are emitted as ANSI cursor/misc escape sequences.
    if (key & KB_KEY_EXTENDED) {
        switch (key & 0x7F) {
            case 0x47: input_push('\x1B'); input_push('['); input_push('H'); break;
            case 0x48: input_push('\x1B'); input_push('['); input_push('A'); break;
            case 0x49: input_push('\x1B'); input_push('['); input_push('5'); input_push('~'); break;
            case 0x4B: input_push('\x1B'); input_push('['); input_push('D'); break;
            case 0x4D: input_push('\x1B'); input_push('['); input_push('C'); break;
            case 0x4F: input_push('\x1B'); input_push('['); input_push('F'); break;
            case 0x50: input_push('\x1B'); input_push('['); input_push('B'); break;
            case 0x51: input_push('\x1B'); input_push('['); input_push('6'); input_push('~'); break;
            case 0x52: input_push('\x1B'); input_push('['); input_push('2'); input_push('~'); break;
            case 0x53: input_push('\x1B'); input_push('['); input_push('3'); input_push('~'); break;
            default: break;
        }
        return;
    }

    if (key >= 128)
        return;

    uint8_t shift = (mods & (KB_MOD_LSHIFT | KB_MOD_RSHIFT)) != 0;

    char ch = input_keymap[key][shift ? 1 : 0];

    // Ctrl+letter collapses to the matching ASCII control code (e.g. ^C = 0x03).
    if (ctrl && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
        ch = (char)(ch & 0x1F);

    // Alt behaves as a Meta key: prefix the character with ESC.
    if (alt && ch) {
        input_push('\x1B');
    }

    // Caps Lock inverts letter case (upper shifts to lower, lower to upper).
    if (locks & KB_LOCK_CAPS) {
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - ('a' - 'A'));
        else if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch + ('a' - 'A'));
    }

    if (ch)
        input_push(ch);
}

// Drop all queued input.
void input_flush() {
    input_head = 0;
    input_tail = 0;
    input_count = 0;
}

// Number of characters currently queued.
int input_available() {
    return (int)input_count;
}

// Pop the oldest queued character; returns -1 when the queue is empty.
int input_getchar(char *c) {
    if (input_count == 0)
        return -1;
    *c = (char)input_buf[input_head];
    input_head = (input_head + 1) % INPUT_BUF_SIZE;
    input_count--;
    return 0;
}

// Reset the input queue and start the keyboard with our event handler.
void input_init() {
    input_head = 0;
    input_tail = 0;
    input_count = 0;
    evdev_init();
    keyboard_set_event_cb(input_handle_event);
    keyboard_init();
    mouse_init();
}
