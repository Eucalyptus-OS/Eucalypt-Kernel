#pragma once

// Populate the IDT (stubs live in idt_stubs.asm) and load it via LIDT.
void idt_init();