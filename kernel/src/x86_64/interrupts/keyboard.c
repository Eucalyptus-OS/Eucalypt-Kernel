#include <x86_64/interrupts/keyboard.h>

#include <shell.h>
#include <x86_64/commands.h>
#include <x86_64/serial.h>
#include <flanterm/flanterm.h>
#include <stdbool.h>

extern struct flanterm_context *ft_ctx;

bool caps_on = false;
bool caps_lock = false;

const uint32_t UNKNOWN = 0xFFFFFFFF;
const uint32_t ESC = 0xFFFFFFFF - 1;
const uint32_t CTRL = 0xFFFFFFFF - 2;
const uint32_t LSHFT = 0xFFFFFFFF - 3;
const uint32_t RSHFT = 0xFFFFFFFF - 4;
const uint32_t ALT = 0xFFFFFFFF - 5;
const uint32_t F1 = 0xFFFFFFFF - 6;
const uint32_t F2 = 0xFFFFFFFF - 7;
const uint32_t F3 = 0xFFFFFFFF - 8;
const uint32_t F4 = 0xFFFFFFFF - 9;
const uint32_t F5 = 0xFFFFFFFF - 10;
const uint32_t F6 = 0xFFFFFFFF - 11;
const uint32_t F7 = 0xFFFFFFFF - 12;
const uint32_t F8 = 0xFFFFFFFF - 13;
const uint32_t F9 = 0xFFFFFFFF - 14;
const uint32_t F10 = 0xFFFFFFFF - 15;
const uint32_t F11 = 0xFFFFFFFF - 16;
const uint32_t F12 = 0xFFFFFFFF - 17;
const uint32_t SCRLCK = 0xFFFFFFFF - 18;
const uint32_t HOME = 0xFFFFFFFF - 19;
const uint32_t UP = 0xFFFFFFFF - 20;
const uint32_t LEFT = 0xFFFFFFFF - 21;
const uint32_t RIGHT = 0xFFFFFFFF - 22;
const uint32_t DOWN = 0xFFFFFFFF - 23;
const uint32_t PGUP = 0xFFFFFFFF - 24;
const uint32_t PGDOWN = 0xFFFFFFFF - 25;
const uint32_t END = 0xFFFFFFFF - 26;
const uint32_t INS = 0xFFFFFFFF - 27;
const uint32_t DEL = 0xFFFFFFFF - 28;
const uint32_t CAPS = 0xFFFFFFFF - 29;
const uint32_t NONE = 0xFFFFFFFF - 30;
const uint32_t ALTGR = 0xFFFFFFFF - 31;
const uint32_t NUMLCK = 0xFFFFFFFF - 32;


const uint32_t lowercase[128] = {
    UNKNOWN,ESC,'1','2','3','4','5','6','7','8',
    '9','0','-','=','\b','\t','q','w','e','r',
    't','y','u','i','o','p','[',']','\n',CTRL,
    'a','s','d','f','g','h','j','k','l',';',
    '\'','`',LSHFT,'\\','z','x','c','v','b','n','m',',',
    '.','/',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',LEFT,UNKNOWN,RIGHT,
    '+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,UNKNOWN,F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
    UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
    UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
    UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

const uint32_t uppercase[128] = {
    UNKNOWN,ESC,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t','Q','W','E','R',
    'T','Y','U','I','O','P','{','}','\n',CTRL,'A','S','D','F','G','H','J','K','L',':','"','~',LSHFT,'|','Z','X','C',
    'V','B','N','M','<','>','?',RSHFT,'*',ALT,' ',CAPS,F1,F2,F3,F4,F5,F6,F7,F8,F9,F10,NUMLCK,SCRLCK,HOME,UP,PGUP,'-',
    LEFT,UNKNOWN,RIGHT,'+',END,DOWN,PGDOWN,INS,DEL,UNKNOWN,UNKNOWN,UNKNOWN,F11,F12,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
    UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
    UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,
    UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN,UNKNOWN
};

#define KBD_BUF_SIZE 256
#define KB_DATA_PORT 0x60
#define KB_STATUS_PORT 0x64
#define KB_COMMAND_PORT 0x64
#define KB_STATUS_OUTPUT_FULL 0x01
#define KB_STATUS_INPUT_FULL 0x02
#define KB_ENABLE_KEYBOARD 0xAE

void init_keyboard() {
    while (inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL) {
        inb(KB_DATA_PORT);
        for (volatile int i = 0; i < 1000; i++);
    }
    
    while (inb(KB_STATUS_PORT) & KB_STATUS_INPUT_FULL);
    outb(KB_COMMAND_PORT, 0xAD);
    for (volatile int i = 0; i < 10000; i++);
    
    while (inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL) {
        inb(KB_DATA_PORT);
        for (volatile int i = 0; i < 1000; i++);
    }
    
    while (inb(KB_STATUS_PORT) & KB_STATUS_INPUT_FULL);
    outb(KB_COMMAND_PORT, KB_ENABLE_KEYBOARD);
    for (volatile int i = 0; i < 10000; i++);
    
    while (inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL) {
        inb(KB_DATA_PORT);
        for (volatile int i = 0; i < 1000; i++);
    }
    
    const int max_retries = 10;
    int attempt;
    
    for (attempt = 0; attempt < max_retries; attempt++) {
        int timeout = 100000;
        while ((inb(KB_STATUS_PORT) & KB_STATUS_INPUT_FULL) && --timeout > 0);
        
        if (timeout <= 0) {
            for (volatile int i = 0; i < 10000; i++);
            continue;
        }

        outb(KB_DATA_PORT, 0xFF);
        
        for (volatile int i = 0; i < 100000; i++);
        
        timeout = 200000;
        while (!(inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL) && --timeout > 0);
        
        if (timeout <= 0) {
            for (volatile int i = 0; i < 10000; i++);
            continue;
        }

        uint8_t resp = inb(KB_DATA_PORT);
        
        if (resp == 0xFA) {
            timeout = 200000;
            while (!(inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL) && --timeout > 0);
            if (timeout > 0) {
                resp = inb(KB_DATA_PORT);
                if (resp == 0xAA) {
                    break;
                }
            }
        } else if (resp == 0xAA) {
            break;
        }
        
        for (volatile int i = 0; i < 10000; i++);
    }
    
    for (attempt = 0; attempt < max_retries; attempt++) {
        int timeout = 100000;
        while ((inb(KB_STATUS_PORT) & KB_STATUS_INPUT_FULL) && --timeout > 0);
        
        if (timeout <= 0) {
            for (volatile int i = 0; i < 10000; i++);
            continue;
        }

        outb(KB_DATA_PORT, 0xF4);
        
        for (volatile int i = 0; i < 50000; i++);
        
        timeout = 100000;
        while (!(inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL) && --timeout > 0);
        
        if (timeout <= 0) {
            for (volatile int i = 0; i < 10000; i++);
            continue;
        }

        uint8_t resp = inb(KB_DATA_PORT);
        
        if (resp == 0xFA) {
            break;
        } else if (resp == 0xFE) {
            continue;
        }
    }
    
    while (inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL) {
        inb(KB_DATA_PORT);
    }
}

void keyboard_handler() {
    if (!(inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL)) {
        return;
    }
    
    uint8_t raw = inb(KB_DATA_PORT);
    bool pressed = (raw & 0x80) == 0;
    uint8_t scancode = raw & 0x7F;

    switch (scancode) {
        case 1:
            break;
        case 14:
            if (pressed) {
                handle_backspace();
            }
            break;
        case 29:
        case 56:
        case 59:
        case 60:
        case 61:
        case 62:
        case 63:
        case 64:
        case 65:
        case 67:
        case 68:
        case 87:
        case 88:
            break;
        case 42:
            if (pressed) {
                caps_on = true;
            } else {
                caps_on = false;
            }
            break;
        case 58:
            if (pressed) {
                caps_lock = !caps_lock;
            }
            break;
        default:
            if (pressed) {
                uint32_t val = (caps_on || caps_lock) ? uppercase[scancode] : lowercase[scancode];
                if (val != UNKNOWN && val != CAPS && val != LSHFT && val != RSHFT && val != CTRL && val != ALT) {
                    shell_print(val);
                }
            }
            break;
    }
}