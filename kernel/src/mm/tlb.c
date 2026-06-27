#include <stdint.h>
#include <stddef.h>
#include <idt/idt.h>
#include <smp.h>
#include <sync/spinlock.h>
#include <interrupts/apic.h>
#include <mm/tlb.h>

static spinlock_t shootdown_lock = 0;
static volatile uint64_t shootdown_addr;
static volatile size_t   shootdown_len;
static volatile int      shootdown_acks;

void invalidate(uint64_t addr, size_t length) {
    for (; length > 0; length -= 0x1000, addr += 0x1000)
        asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

void tlb_shootdown_interrupt() {
    invalidate(shootdown_addr, shootdown_len);
    __atomic_fetch_add(&shootdown_acks, 1, __ATOMIC_SEQ_CST);
    apic_eoi();
}

void tlb_shootdown(uint64_t addr, size_t length) {
    if (!addr || !length)
        return;

    spinlock_acquire(&shootdown_lock);

    invalidate(addr, length);

    if (smp_online_count() > 1) {
        shootdown_addr = addr;
        shootdown_len  = length;
        __atomic_store_n(&shootdown_acks, 0, __ATOMIC_SEQ_CST);

        int targets = 0;
        uint8_t self_id = apic_id();
        for (int i = 0; i < cpu_count; i++) {
            if (!smp_is_cpu_online(cpus[i].cpu_id))
                continue;

            uint8_t target_id = smp_get_apic_id(cpus[i].cpu_id);
            if (target_id == self_id)
                continue;

            apic_send_ipi(target_id, TLB_SHOOTDOWN_VECTOR);
            targets++;
        }

        while (__atomic_load_n(&shootdown_acks, __ATOMIC_SEQ_CST) < targets)
            asm volatile("pause");
    }

    spinlock_release(&shootdown_lock);
}
