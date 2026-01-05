#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

void shell_init();
void shell_print(uint32_t v);
static inline void handle_backspace() {
    shell_print('\b');
}

#endif