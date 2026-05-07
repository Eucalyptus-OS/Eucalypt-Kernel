#include <stddef.h>
#include <stdint.h>

void invalidate(uint64_t addr, size_t length) {
    for(; length > 0; length -= 0x1000, addr += 0x1000) asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}
