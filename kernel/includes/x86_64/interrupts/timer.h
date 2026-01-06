#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void on_irq0();
void timer_wait(uint32_t ticks);
void timer_wait_ms(uint32_t milliseconds);
void init_timer();
uint64_t get_timer_ticks();

#endif