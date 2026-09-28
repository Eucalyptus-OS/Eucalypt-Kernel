#include <stdint.h>
#include <portio.h>
#include <abi/errno.h>
#include <abi/syscalls.h>

// QEMU's PIIX4 ACPI PM block. The PM base is hardcoded at 0x600 by QEMU, and the
// power-management control register PM1a_CNT sits at PMBA + 0x04. Writing a sleep
// type into SLP_TYP with SLP_EN set makes the firmware perform that transition.
//
// This is deliberately not a real ACPI implementation: SLP_TYP values are normally
// resolved by interpreting the _S5 package in the DSDT, and PMBA has to be read out
// of the FADT. QEMU is the only platform this OS boots on today and it hardcodes
// both, so the addresses and the S5/S6 encodings are inlined. On hardware without
// the PIIX4 block the port write goes nowhere and the call is a silent no-op.
#define PM1a_CNT_PORT   0x0604
#define SLP_EN          (1u << 13)
#define SLP_TYP_SHIFT   9

// S5 (soft power off) and S6 (soft reboot) sleep-type encodings, as emitted by
// QEMU's DSDT for the PIIX4.
#define SLP_TYP_S5      0
#define SLP_TYP_S6      2

// Enter a sleep state. Does not return on success; the machine is going down.
static void enter_sleep_state(unsigned int type) {
    // Mask in the existing SLP_EN/SLP_TYP bits so a stale status read cannot
    // leave us requesting a different transition than intended.
    uint16_t cnt = inw(PM1a_CNT_PORT);
    cnt = (uint16_t)((cnt & ~((0x3u << SLP_TYP_SHIFT) | SLP_EN))
                     | SLP_EN | ((type & 0x3u) << SLP_TYP_SHIFT));
    outw(PM1a_CNT_PORT, cnt);
}

// Reboot or power off the machine. cmd is one of the RB_* values in abi/syscalls.h,
// matching Linux's reboot(2) argument.
uint64_t sys_reboot(int cmd) {
    unsigned int type;

    switch (cmd) {
        case RB_POWER_OFF:
            type = SLP_TYP_S5;
            break;
        case RB_AUTOBOOT:
            type = SLP_TYP_S6;
            break;
        // Linux can stop the CPU without cutting power, but that needs S1 and a
        // real PM block. Refuse rather than silently powering off instead.
        case RB_HALT_SYSTEM:
            return (uint64_t)(-ENOSYS);
        default:
            return (uint64_t)(-EINVAL);
    }

    enter_sleep_state(type);

    // The firmware is expected to take the machine down here. If it did not, spin
    // rather than returning to a caller that has already torn down its session.
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
