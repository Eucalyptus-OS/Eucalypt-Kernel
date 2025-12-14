#include <x86_64/commands.h>

#include <stdint.h>

uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile (
        "inb %w1, %b0": "=a" (result): "Nd" (port): "memory"
    );
    return result;
}

void outb(uint16_t port, uint8_t val) {
    __asm__ volatile (
        "outb %b0, %w1":: "a" (val),"Nd" (port): "memory"
    );
}

void outw(uint16_t port, uint16_t val) {
    __asm__ volatile (
        "outw %w0, %w1":: "a" (val), "Nd" (port): "memory"
    );
}

uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ volatile (
        "inw %w1, %w0": "=a" (result): "Nd" (port): "memory"
    );
    return result;
}