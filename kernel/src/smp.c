#include <stdbool.h>
#include <limine.h>
#include <logging/printk.h>
#include <mem.h>
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
extern void enable_sse();

static volatile uint8_t ap_stage[100];
static volatile uint8_t cpu_online[100];
static volatile uint8_t ap_console_printed[100];
static volatile uint8_t online_count = 0;
static volatile bool ap_console_released = false;

#define MAX_CPUS 100
#define SMP_CONSOLE_MAX_CPUS 24
#define AP_STARTUP_TIMEOUT 100000000ULL

enum {
    AP_STAGE_OFFLINE = 0,
    AP_STAGE_ENTERED = 1,
    AP_STAGE_GDT = 2,
    AP_STAGE_IDT = 3,
    AP_STAGE_PAGING = 4,
    AP_STAGE_APIC = 5,
    AP_STAGE_TIMER = 6,
    AP_STAGE_READY = 7,
};

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

static const uint8_t cpu_letters[24] __attribute__((unused)) = {
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

static void smp_mark_cpu_online(uint8_t pid) {
    if (pid >= MAX_CPUS) {
        return;
    }
    if (__atomic_exchange_n(&cpu_online[pid], 1, __ATOMIC_ACQ_REL) == 0) {
        __atomic_fetch_add(&online_count, 1, __ATOMIC_ACQ_REL);
    }
}

bool smp_is_cpu_online(uint8_t pid) {
    if (pid >= MAX_CPUS) {
        return false;
    }
    return __atomic_load_n(&cpu_online[pid], __ATOMIC_ACQUIRE) != 0;
}

uint8_t smp_online_count(void) {
    return __atomic_load_n(&online_count, __ATOMIC_ACQUIRE);
}

void ap_entry(uint64_t pid) {
    asm volatile ("cli");
    bool printed_online = false;

    if (pid >= MAX_CPUS) {
        for (;;) {
            asm volatile ("cli\nhlt");
        }
    }

    __atomic_store_n(&ap_stage[pid], AP_STAGE_ENTERED, __ATOMIC_RELEASE);

    if (per_cpu_data[pid]) {
        gdt_init_percpu(per_cpu_data[pid]);
    }
    __atomic_store_n(&ap_stage[pid], AP_STAGE_GDT, __ATOMIC_RELEASE);

    idt_init_per_cpu();
    __atomic_store_n(&ap_stage[pid], AP_STAGE_IDT, __ATOMIC_RELEASE);

    paging_init_per_cpu();
    __atomic_store_n(&ap_stage[pid], AP_STAGE_PAGING, __ATOMIC_RELEASE);

    enable_apic(pid, false);
    __atomic_store_n(&ap_stage[pid], AP_STAGE_APIC, __ATOMIC_RELEASE);

    enable_sse();

    apic_timer_init(1000);
    __atomic_store_n(&ap_stage[pid], AP_STAGE_TIMER, __ATOMIC_RELEASE);

    smp_mark_cpu_online(pid);
    __atomic_store_n(&ap_stage[pid], AP_STAGE_READY, __ATOMIC_RELEASE);
    asm volatile ("sti");

    for (;;) {
        asm volatile ("hlt" ::: "memory");
        if (!printed_online &&
            __atomic_load_n(&ap_console_released, __ATOMIC_ACQUIRE) &&
            pid < SMP_CONSOLE_MAX_CPUS) {
            println(pid, "Hello world", cpu_colors[pid]);
            __atomic_store_n(&ap_console_printed[pid], 1, __ATOMIC_RELEASE);
            printed_online = true;
        }
    }
}

uint8_t smp_get_apic_id(uint8_t pid) {
    struct limine_mp_response *mp_response = mp_request.response;
    if (!mp_response) {
        return 0;
    }

    for (int i = 0; i < cpu_count; i++) {
        if (mp_response->cpus[i]->processor_id == pid) {
            return mp_response->cpus[i]->lapic_id;
        }
    }
    return 0;
}

uint8_t smp_get_cpu_id(uint8_t lapic_id) {
    for (int i = 0; i < cpu_count; i++) {
        if (cpus[i].lapic_id == lapic_id) {
            return cpus[i].cpu_id;
        }
    }
    return lapic_id;
}

uint8_t smp_current_cpu_id(void) {
    return smp_get_cpu_id(apic_id());
}

uint8_t smp_init() {
    struct limine_mp_response *mp_response = mp_request.response;
    if (!mp_response || mp_response->cpu_count > MAX_CPUS) {
        return 1;
    }

    cpu_count = mp_response->cpu_count;

    bsp_lapic_id = mp_response->bsp_lapic_id;
    uint8_t bsp_pid = 0;
    for (int i = 0; i < cpu_count; i++) {
        if (mp_response->cpus[i]->lapic_id == bsp_lapic_id) {
            bsp_pid = mp_response->cpus[i]->processor_id;
            break;
        }
    }
    for (int i = 0; i < MAX_CPUS; i++) {
        __atomic_store_n(&cpu_online[i], 0, __ATOMIC_RELEASE);
        __atomic_store_n(&ap_stage[i], AP_STAGE_OFFLINE, __ATOMIC_RELEASE);
        __atomic_store_n(&ap_console_printed[i], 0, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&online_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ap_console_released, false, __ATOMIC_RELEASE);
    smp_mark_cpu_online(bsp_pid);

    for (int i = 0; i < cpu_count; i++) {
        uint8_t pid = mp_response->cpus[i]->processor_id;
        uint8_t lid = mp_response->cpus[i]->lapic_id;
        uint64_t reserved = mp_response->cpus[i]->reserved;
        if (pid >= MAX_CPUS) {
            log_debug("CPU processor ID %d exceeds MAX_CPUS\n", pid);
            return 1;
        }

        cpus[i] = (cpu_t){
            .cpu_id = pid,
            .lapic_id = lid,
            .reserved = reserved,
        };
        log_debug("\n\n\rProcessor ID: %d\n\rLapic ID: %d\n\rreserved: %lX\n\n",
                           pid, lid, reserved);
    }

    for (int i = 0; i < cpu_count; i++) {
        uint8_t pid = mp_response->cpus[i]->processor_id;

        if (pid == bsp_pid) {
            __atomic_store_n(&ap_stage[pid], AP_STAGE_READY, __ATOMIC_RELEASE);
            continue;
        }

        __atomic_store_n(&ap_stage[pid], AP_STAGE_OFFLINE, __ATOMIC_RELEASE);

        per_cpu_data[pid] = (gdt_per_cpu_t *)kmalloc(sizeof(gdt_per_cpu_t));
        if (!per_cpu_data[pid]) {
            log_debug("Failed to allocate per-CPU data for CPU %d\n", pid);
            return 1;
        }
        memset(per_cpu_data[pid], 0, sizeof(gdt_per_cpu_t));

        uint8_t *stack = kmalloc(KERNEL_STACK_SIZE + 16);
        if (!stack) {
            log_debug("Failed to allocate startup stack for CPU %d\n", pid);
            return 1;
        }
        uint64_t stack_top = (uint64_t)(stack + KERNEL_STACK_SIZE + 16);
        stack_top &= ~0xFULL;
        ap_stack_tops[pid] = stack_top;

        mp_response->cpus[i]->extra_argument = pid;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        mp_response->cpus[i]->goto_address = (limine_goto_address)smp_trampoline;

        uint64_t timeout = AP_STARTUP_TIMEOUT;
        while (__atomic_load_n(&ap_stage[pid], __ATOMIC_ACQUIRE) != AP_STAGE_READY && timeout--) {
            asm volatile ("pause");
        }
        if (__atomic_load_n(&ap_stage[pid], __ATOMIC_ACQUIRE) != AP_STAGE_READY) {
            log_debug("CPU %d failed to start, stage=%d\n",
                      pid, __atomic_load_n(&ap_stage[pid], __ATOMIC_ACQUIRE));
            return 1;
        }
    }

    if (cpu_count <= SMP_CONSOLE_MAX_CPUS) {
        printk_set_framebuffer_enabled(false);
        if (smp_console_init(cpu_count) != 0) {
            printk_set_framebuffer_enabled(true);
            return 1;
        }
        if (bsp_pid < SMP_CONSOLE_MAX_CPUS) {
            println(bsp_pid, "Hello world", cpu_colors[bsp_pid]);
            __atomic_store_n(&ap_console_printed[bsp_pid], 1, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&ap_console_released, true, __ATOMIC_RELEASE);

        for (int i = 0; i < cpu_count; i++) {
            uint8_t pid = mp_response->cpus[i]->processor_id;
            if (pid >= SMP_CONSOLE_MAX_CPUS) {
                continue;
            }

            uint64_t timeout = AP_STARTUP_TIMEOUT;
            while (!__atomic_load_n(&ap_console_printed[pid], __ATOMIC_ACQUIRE) && timeout--) {
                asm volatile ("pause");
            }
        }
    }

    return 0;
}
