#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

#define COM1_PORT 0x3F8

#define UART_DATA        0x00
#define UART_INTR_EN     0x01
#define UART_INTR_ID     0x02
#define UART_LINE_CTRL   0x03
#define UART_MODEM_CTRL  0x04
#define UART_LINE_STATUS 0x05

void serial_init();
void serial_putchar(char c);
void serial_print_num(uint64_t num);
void serial_print_hex(uint64_t num);
void serial_print(const char *str);

#endif
