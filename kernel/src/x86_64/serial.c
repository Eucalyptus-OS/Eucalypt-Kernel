#include <x86_64/serial.h>
#include <x86_64/commands.h>

void serial_init() {
    outb(COM1_PORT + UART_INTR_EN, 0x00);
    
    outb(COM1_PORT + UART_LINE_CTRL, 0x80);
    
    outb(COM1_PORT + UART_DATA, 0x01);
    outb(COM1_PORT + UART_DATA + 1, 0x00);
    
    outb(COM1_PORT + UART_LINE_CTRL, 0x03);
    
    outb(COM1_PORT + UART_MODEM_CTRL, 0x03);
}

void serial_putchar(char c) {
    while (!(inb(COM1_PORT + UART_LINE_STATUS) & 0x20)) {
        asm volatile("pause");
    }
    outb(COM1_PORT + UART_DATA, (uint8_t)c);
}

void serial_print_num(uint64_t num) {
    char buffer[21];
    int i = 0;

    if (num == 0) {
        serial_putchar('0');
        return;
    }

    while (num > 0) {
        buffer[i++] = '0' + (num % 10);
        num /= 10;
    }

    for (int j = i - 1; j >= 0; j--) {
        serial_putchar(buffer[j]);
    }
}

void serial_print(const char *str) {
    while (*str) {
        if (*str == '\n') {
            serial_putchar('\r');
        }
        serial_putchar(*str++);
    }
}

void serial_print_hex(uint64_t num) {
    serial_print("0x");
    if (num == 0) {
        serial_putchar('0');
        return;
    }

    char buf[16];
    int i = 0;
    while (num > 0) {
        uint8_t nib = num & 0xF;
        if (nib < 10) buf[i++] = '0' + nib;
        else buf[i++] = 'a' + (nib - 10);
        num >>= 4;
    }

    for (int j = i - 1; j >= 0; j--) serial_putchar(buf[j]);
}
