#include <stdbool.h>
#include <limine.h>
#include <logging/printk.h>
#include <mm/heap.h>
#include <mm/paging.h>
#include <multitasking/thread.h>
#include <gdt/gdt.h>
#include <idt/idt.h>
#include <interrupts/apic.h>
#include <portio.h>
#include <msr.h>
#include <smp.h>

__attribute__((used, section(".limine_requests")))
volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0
};

cpu_t cpus[100];
uint64_t ap_stack_tops[100];
extern void smp_trampoline();

void ap_entry(uint64_t pid) {
    asm volatile ("cli");
    uint64_t timer_ticks = 0;

    if (per_cpu_data[pid]) {
        gdt_init_percpu(per_cpu_data[pid]);
    }

    idt_init_per_cpu();
    paging_init_per_cpu();
    enable_apic(pid, false);
    apic_timer_init(1000);
    log_debug("Hello from processor: %d\n", pid);
    asm volatile ("sti");
    for (;;) {
        asm volatile ("hlt" ::: "memory");
        timer_ticks++;
        if (timer_ticks == 1000) {
            log_debug("AP %llu heartbeat\n", (unsigned long long)pid);
            timer_ticks = 0;
        }
    }
}

uint8_t smp_init() {
    struct limine_mp_response *mp_response = mp_request.response;
    uint8_t cpu_count = mp_response->cpu_count;

    uint8_t bsp_lapic_id = mp_response->bsp_lapic_id;
    uint8_t bsp_pid = 0;
    for (int i = 0; i < cpu_count; i++) {
        if (mp_response->cpus[i]->lapic_id == bsp_lapic_id) {
            bsp_pid = mp_response->cpus[i]->processor_id;
            break;
        }
    }

    for (int i = 0; i < cpu_count; i++) {
        uint8_t pid = mp_response->cpus[i]->processor_id;
        uint8_t lid = mp_response->cpus[i]->lapic_id;
        uint64_t reserved = mp_response->cpus[i]->reserved;

        cpus[i] = (cpu_t){
            .cpu_id = pid,
            .lapic_id = lid,
            .reserved = reserved,
        };
        log_debug("\n\n\rProcessor ID: %d\n\rLapic ID: %d\n\rreserved: %lX\n\n",
                           pid, lid, reserved);

        if (pid == bsp_pid) {
            continue;
        }

        per_cpu_data[pid] = (gdt_per_cpu_t *)kmalloc(sizeof(gdt_per_cpu_t));
        if (!per_cpu_data[pid]) {
            log_debug("Failed to allocate per-CPU data for CPU %d\n", pid);
            return 1;
        }
        uint8_t *stack = kmalloc(KERNEL_STACK_SIZE + 16);
        uint64_t stack_top = (uint64_t)(stack + KERNEL_STACK_SIZE + 16);
        stack_top &= ~0xFULL;
        ap_stack_tops[pid] = stack_top;
        mp_response->cpus[i]->goto_address = (limine_goto_address)smp_trampoline;
        mp_response->cpus[i]->extra_argument = pid;
    }
    return 0;
}
