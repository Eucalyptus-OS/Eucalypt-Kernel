#pragma once

// Reset, configure, and probe the PS/2 mouse, then arm IRQ12 at vector 0x23.
void mouse_init();

// PS/2 mouse IRQ handler (vector 0x23): parse packets into /dev/event1.
void mouse_handler();