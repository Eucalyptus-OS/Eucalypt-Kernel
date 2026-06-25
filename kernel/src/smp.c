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
#include <logging/smp_console.h>
#include <msr.h>
#include <smp.h>

__attribute__((used, section(".limine_requests")))
volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0
};

uint8_t cpu_count = 0;
uint8_t bsp_lapic_id = 0;
cpu_t cpus[100];
uint64_t ap_stack_tops[100];
extern void smp_trampoline();

static const uint32_t cpu_colors[24] = {
    0xFFFFFFFF, // 0 white
    0xFFFF0000, // 1 red
    0xFF00FF00, // 2 green
    0xFF0000FF, // 3 blue
    0xFFFFFF00, // 4 yellow
    0xFFFF00FF, // 5 magenta
    0xFF00FFFF, // 6 cyan
    0xFF808080, // 7 gray (was mislabeled "black" with wrong alpha before)
    0xFFFF8000, // 8 orange
    0xFF8000FF, // 9 purple
    0xFF0080FF, // 10 sky blue
    0xFF80FF00, // 11 lime
    0xFFFF0080, // 12 pink
    0xFF00FF80, // 13 mint
    0xFF804000, // 14 brown
    0xFFC0C0C0, // 15 light gray
    0xFF400080, // 16 indigo
    0xFF008040, // 17 teal-green
    0xFFFF4040, // 18 light red
    0xFF40FF40, // 19 light green
    0xFF4040FF, // 20 light blue
    0xFFFFC040, // 21 gold
    0xFF40FFC0, // 22 aqua
    0xFFC040FF, // 23 violet
};

static const uint8_t cpu_letters[24] = {
    'A', // 0 white
    'B', // 1 red
    'C', // 2 green
    'D', // 3 blue
    'E', // 4 yellow
    'F', // 5 magenta
    'G', // 6 cyan
    'H', // 7 gray (was mislabeled "black" with wrong alpha before)
    'I', // 8 orange
    'J', // 9 purple
    'K', // 10 sky blue
    'L', // 11 lime
    'M', // 12 pink
    'N', // 13 mint
    'O', // 14 brown
    'P', // 15 light gray
    'Q', // 16 indigo
    'R', // 17 teal-green
    'S', // 18 light red
    'T', // 19 light green
    'U', // 20 light blue
    'V', // 21 gold
    'W', // 22 aqua
    'X', // 23 violet
};

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
    //log_debug("Hello from processor: %d\n", pid);
    asm volatile ("sti");
    for (;;) {
        asm volatile ("hlt" ::: "memory");
        timer_ticks++;
        if (timer_ticks == 1) {
            if (pid <= 24) {
                smp_console_draw_glyph(pid, cpu_colors[pid], cpu_letters[pid]);
            }
            timer_ticks = 0;
        }
    }
}

uint8_t smp_init() {
    struct limine_mp_response *mp_response = mp_request.response;
    cpu_count = mp_response->cpu_count;

    smp_console_init(cpu_count);

    bsp_lapic_id = mp_response->bsp_lapic_id;
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
