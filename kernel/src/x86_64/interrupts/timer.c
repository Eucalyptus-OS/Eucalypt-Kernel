#include <x86_64/interrupts/timer.h>

#include <x86_64/commands.h>
#include <x86_64/interrupts/pic.h>

static volatile uint64_t timer_ticks = 0;

void on_irq0() {
    timer_ticks++;
}

void timer_wait(uint32_t ticks) {
    uint64_t start_ticks = timer_ticks;
    while (timer_ticks < start_ticks + ticks) {
        __asm__ volatile ("hlt");
    }
}

void timer_wait_ms(uint32_t milliseconds) {
    uint64_t target = get_timer_ticks() + milliseconds;
    
    while (get_timer_ticks() < target) {
        __asm__ volatile ("hlt");
    }
}

void init_timer() {
    uint16_t divisor = 1193;
    
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

uint64_t get_timer_ticks() {
    return timer_ticks;
}