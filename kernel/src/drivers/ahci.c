#include <stdint.h>
#include <logging/print.h>
#include <mm/hhdm.h>
#include <mm/paging.h>
#include <memory.h>
#include <drivers/pci.h>
#include <apic.h>
#include <drivers/ahci.h>

#define AHCI_GHC_AE   (1u << 31)
#define AHCI_GHC_IE   (1u << 1)
#define AHCI_GHC_HR   (1u << 0)

#define AHCI_BOHC_BOS (1u << 0)
#define AHCI_BOHC_OOS (1u << 1)
#define AHCI_BOHC_SOOE (1u << 2)
#define AHCI_BOHC_OOC (1u << 3)
#define AHCI_BOHC_BB  (1u << 4)

#define AHCI_CAP2_BOH (1u << 0)

#define AHCI_SPIN_TIMEOUT_ITERS 2000000

static HBA_MEM *g_abars[AHCI_MAX_CONTROLLERS];
static uint8_t  g_abar_count = 0;
static ahci_state_t g_ahci;
static drive_t g_drives[AHCI_MAX_CONTROLLERS][AHCI_MAX_PORTS];
static uint8_t g_identify_buf[512] __attribute__((aligned(4096)));

typedef struct {
    ahci_callback_t callback;
    void *ctx;
} ahci_pending_t;

static ahci_pending_t g_pending[AHCI_MAX_CONTROLLERS][AHCI_MAX_PORTS][AHCI_MAX_CMD_SLOTS];
static uint32_t g_pending_mask[AHCI_MAX_CONTROLLERS][AHCI_MAX_PORTS];

static int check_type(HBA_PORT *port) {
    uint32_t ssts = port->ssts;

    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    print("port status ssts=0x%x ipm=0x%x det=0x%x sig=0x%x\n", ssts, ipm, det, port->sig);

    if (det != HBA_PORT_DET_PRESENT) {
        print("device not present\n");
        return AHCI_DEV_NULL;
    }
    if (ipm != HBA_PORT_IPM_ACTIVE) {
        print("device not active\n");
        return AHCI_DEV_NULL;
    }

    switch (port->sig) {
        case HBA_PORT_SIG_ATAPI:
            print("device type SATAPI\n");
            return AHCI_DEV_SATAPI;
        case HBA_PORT_SIG_SEMB:
            print("device type SEMB\n");
            return AHCI_DEV_SEMB;
        case HBA_PORT_SIG_PM:
            print("device type PM\n");
            return AHCI_DEV_PM;
        default:
            print("device type SATA\n");
            return AHCI_DEV_SATA;
    }
}

static void ahci_spin_delay(uint32_t iters) {
    for (volatile uint32_t i = 0; i < iters; i++) {
        asm volatile ("pause");
    }
}

static void ahci_bios_handoff(HBA_MEM *abar) {
    print("bios handoff cap2=0x%x bohc=0x%x\n", abar->cap2, abar->bohc);

    if (!(abar->cap2 & AHCI_CAP2_BOH)) {
        print("bios handoff not supported, skipping\n");
        return;
    }

    abar->bohc |= AHCI_BOHC_OOS;
    print("requested OS ownership, bohc=0x%x\n", abar->bohc);

    uint32_t spin = 0;
    while ((abar->bohc & AHCI_BOHC_BOS) && spin++ < AHCI_SPIN_TIMEOUT_ITERS) {
        ahci_spin_delay(1);
    }

    print("wait for BIOS ownership done after %u spins, bohc=0x%x\n", spin, abar->bohc);

    ahci_spin_delay(250000);

    if (abar->bohc & AHCI_BOHC_BB) {
        print("BIOS busy set, extra delay\n");
        ahci_spin_delay(2000000);
    }

    abar->bohc |= AHCI_BOHC_OOC;
    print("bios handoff complete, bohc=0x%x\n", abar->bohc);
}

static uint8_t ahci_hba_enable(HBA_MEM *abar) {
    print("enabling HBA, ghc=0x%x\n", abar->ghc);

    abar->ghc |= AHCI_GHC_AE;

    uint32_t spin = 0;
    while (!(abar->ghc & AHCI_GHC_AE)) {
        if (++spin > AHCI_SPIN_TIMEOUT_ITERS) {
            print("HBA enable timed out, ghc=0x%x\n", abar->ghc);
            return 1;
        }
    }

    print("HBA enabled after %u spins, ghc=0x%x\n", spin, abar->ghc);
    return 0;
}

static uint8_t start_cmd(HBA_PORT *port) {
    print("starting command engine, cmd=0x%x\n", port->cmd);

    uint32_t spin = 0;
    while (port->cmd & HBA_PxCMD_CR) {
        if (++spin > AHCI_SPIN_TIMEOUT_ITERS) {
            print("timed out waiting for CR clear, cmd=0x%x\n", port->cmd);
            return 1;
        }
    }

    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
    print("command engine started, cmd=0x%x\n", port->cmd);
    return 0;
}

static uint8_t stop_cmd(HBA_PORT *port) {
    print("stopping command engine, cmd=0x%x\n", port->cmd);

    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;

    uint32_t spin = 0;
    while (1) {
        if (!(port->cmd & HBA_PxCMD_FR) && !(port->cmd & HBA_PxCMD_CR)) {
            break;
        }
        if (++spin > AHCI_SPIN_TIMEOUT_ITERS) {
            print("timed out stopping command engine, cmd=0x%x\n", port->cmd);
            return 1;
        }
    }

    print("command engine stopped after %u spins, cmd=0x%x\n", spin, port->cmd);
    return 0;
}

static int port_rebase(HBA_PORT *port, int portno) {
    print("rebasing port=%d\n", portno);

    if (stop_cmd(port) != 0) {
        print("port %d failed to stop command engine, skipping\n", portno);
        return 1;
    }

    uint64_t phys_clb = AHCI_BASE + (portno << 10);
    uint64_t virt_clb = (uint64_t)phys_to_virt((uintptr_t)phys_clb);

    // Ensure CLB is aligned to 4096 bytes
    if ((virt_clb & 0xFFF) != 0) {
        phys_clb += 4096 - (virt_clb & 0xFFF);
        virt_clb = (uint64_t)phys_to_virt((uintptr_t)phys_clb);
    }

    port->clb  = (uint32_t)(phys_clb & 0xFFFFFFFF);
    port->clbu = (uint32_t)(phys_clb >> 32);

    memset((void *)virt_clb, 0, 1024);
    print("port %d clb phys=0x%lx virt=0x%lx\n", portno, (unsigned long)phys_clb, (unsigned long)virt_clb);

    uint64_t phys_fb = AHCI_BASE + (32 << 10) + (portno << 8);
    uint64_t virt_fb = (uint64_t)phys_to_virt((uintptr_t)phys_fb);

    // Ensure FB is aligned to 4096 bytes
    if ((virt_fb & 0xFFF) != 0) {
        phys_fb += 4096 - (virt_fb & 0xFFF);
        virt_fb = (uint64_t)phys_to_virt((uintptr_t)phys_fb);
    }

    port->fb  = (uint32_t)(phys_fb & 0xFFFFFFFF);
    port->fbu = (uint32_t)(phys_fb >> 32);

    memset((void *)virt_fb, 0, 256);
    print("port %d fb phys=0x%lx virt=0x%lx\n", portno, (unsigned long)phys_fb, (unsigned long)virt_fb);

    port->is   = (uint32_t)-1;
    port->ie   = (uint32_t)-1;
    port->serr = (uint32_t)-1;
    port->ci   = 0;
    port->sact = 0;

    HBA_CMD_HEADER *cmdheader = (HBA_CMD_HEADER *)virt_clb;
    for (int i = 0; i < 32; i++) {
        uint64_t phys_ctba = AHCI_BASE + (40 << 10) + (portno << 13) + (i << 8);
        uint64_t virt_ctba = (uint64_t)phys_to_virt((uintptr_t)phys_ctba);

        if ((virt_ctba & 0xFFF) != 0) {
            phys_ctba += 4096 - (virt_ctba & 0xFFF);
            virt_ctba = (uint64_t)phys_to_virt((uintptr_t)phys_ctba);
        }

        cmdheader[i].prdtl = AHCI_CMD_TBL_PRDT_ENTRIES;
        cmdheader[i].ctba  = (uint32_t)(phys_ctba & 0xFFFFFFFF);
        cmdheader[i].ctbau = (uint32_t)(phys_ctba >> 32);
        memset((void *)virt_ctba, 0, 256);
    }
    print("port %d command tables initialized\n", portno);

    if (start_cmd(port) != 0) {
        print("port %d failed to start command engine\n", portno);
        return 1;
    }

    print(" complete port=%d\n", portno);
    return 0;
}

static int find_cmdslot(HBA_PORT *port) {
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < AHCI_MAX_CMD_SLOTS; i++) {
        if ((slots & (1u << i)) == 0) {
            print("found slot %d (slots=0x%x)\n", i, slots);
            return i;
        }
    }
    print("no free slot (slots=0x%x)\n", slots);
    return -1;
}

static uint8_t ahci_identify(uint8_t controller, uint8_t port) {
    print("identify start controller=%u port=%u\n", controller, port);

    if (controller >= g_abar_count || g_abars[controller] == NULL) {
        print("invalid controller %u\n", controller);
        return 1;
    }

    HBA_PORT *hba_port = &g_abars[controller]->ports[port];
    hba_port->is   = (uint32_t)-1;
    hba_port->serr = (uint32_t)-1;

    int slot = find_cmdslot(hba_port);
    if (slot == -1) {
        print("no cmd slot controller=%u port=%u\n", controller, port);
        return 1;
    }

    uint64_t clb_phys = (uint64_t)hba_port->clb | ((uint64_t)hba_port->clbu << 32);
    HBA_CMD_HEADER *cmdheader = (HBA_CMD_HEADER *)phys_to_virt(clb_phys) + slot;

    uint64_t ctba_phys = (uint64_t)cmdheader->ctba | ((uint64_t)cmdheader->ctbau << 32);

    uint32_t saved_ctba  = cmdheader->ctba;
    uint32_t saved_ctbau = cmdheader->ctbau;
    memset(cmdheader, 0, sizeof(*cmdheader));
    cmdheader->ctba  = saved_ctba;
    cmdheader->ctbau = saved_ctbau;
    cmdheader->cfl   = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
    cmdheader->w     = 0;
    cmdheader->prdtl = 1;
    cmdheader->prdbc = 0;

    HBA_CMD_TBL *cmdtbl = (HBA_CMD_TBL *)phys_to_virt(ctba_phys);
    memset(cmdtbl, 0, sizeof(HBA_CMD_TBL));

    memset(g_identify_buf, 0, sizeof(g_identify_buf));
    uint64_t buf_phys = virt_to_phys(g_identify_buf);

    cmdtbl->prdt_entry[0].dba  = (uint32_t)(buf_phys & 0xFFFFFFFF);
    cmdtbl->prdt_entry[0].dbau = (uint32_t)(buf_phys >> 32);
    cmdtbl->prdt_entry[0].dbc  = sizeof(g_identify_buf) - 1;
    cmdtbl->prdt_entry[0].i    = 1;

    FIS_REG_H2D *cfis = (FIS_REG_H2D *)&cmdtbl->cfis;
    memset(cfis, 0, sizeof(*cfis));
    cfis->fis_type = FIS_TYPE_REG_H2D;
    cfis->c        = 1;
    cfis->command  = ATA_CMD_IDENTIFY;
    cfis->device   = 0;
    cfis->countl   = 0;
    cfis->counth   = 0;

    print("identify slot=%d ctba_phys=0x%lx buf_phys=0x%lx tfd=0x%x\n",
             slot, (unsigned long)ctba_phys, (unsigned long)buf_phys, hba_port->tfd);

    uint32_t spin = 0;
    while ((hba_port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) != 0) {
        if (++spin > AHCI_SPIN_TIMEOUT_ITERS) {
            print("timed out waiting for BSY/DRQ clear, tfd=0x%x\n", hba_port->tfd);
            return 1;
        }
    }

    hba_port->ci |= 1u << slot;
    print("issued command, ci=0x%x\n", hba_port->ci);

    spin = 0;
    while ((hba_port->ci & (1u << slot)) != 0) {
        if (hba_port->is & (HBA_PxIS_TFES | HBA_PxIS_HBFS | HBA_PxIS_HBDS | HBA_PxIS_IFS)) {
            print("command error is=0x%x\n", hba_port->is);
            return 1;
        }
        if (++spin > AHCI_SPIN_TIMEOUT_ITERS) {
            print("timed out waiting for completion, ci=0x%x is=0x%x\n", hba_port->ci, hba_port->is);
            return 1;
        }
    }

    if (hba_port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) {
        print("final tfd still busy/drq, tfd=0x%x\n", hba_port->tfd);
        return 1;
    }

    uint16_t *words = (uint16_t *)g_identify_buf;

    uint64_t lba48_sectors =
        (uint64_t)words[100] |
        ((uint64_t)words[101] << 16) |
        ((uint64_t)words[102] << 32) |
        ((uint64_t)words[103] << 48);

    uint32_t sector_size = AHCI_SECTOR_SIZE;
    if ((words[106] & (1 << 14)) && !(words[106] & (1 << 15)) && (words[106] & (1 << 12))) {
        uint32_t words_per_sector = (uint32_t)words[117] | ((uint32_t)words[118] << 16);
        sector_size = words_per_sector * 2;
        print("logical sector size override -> %u\n", sector_size);
    }

    drive_t *drive = &g_drives[controller][port];
    drive->sector_count = lba48_sectors;
    drive->sector_size  = sector_size;
    drive->controller   = controller;
    drive->port         = port;
    drive->device_type  = AHCI_DEV_SATA;

    print("identify complete controller=%u port=%u sectors=%lu sector_size=%u\n",
             controller, port, (unsigned long)lba48_sectors, sector_size);

    return 0;
}

static void ahci_probe_ports(HBA_MEM *abar, uint8_t controller) {
    uint32_t pi = abar->pi;
    print("probing ports controller=%u pi=0x%x\n", controller, pi);

    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if ((pi & (1u << i)) == 0)
            continue;

        HBA_PORT *port = &abar->ports[i];
        int type = check_type(port);

        g_ahci.controllers[controller].ports[i].type = type;
        g_ahci.controllers[controller].ports[i].present = (type != AHCI_DEV_NULL);

        if (type == AHCI_DEV_NULL) {
            print("No device at controller %u port %u\n", controller, i);
            continue;
        }

        if (port_rebase(port, i) != 0) {
            print("rebase failed controller=%u port=%u\n", controller, i);
            g_ahci.controllers[controller].ports[i].present = 0;
            continue;
        }

        switch (type) {
            case AHCI_DEV_SATA:
                print("SATA drive found at controller %u port %u\n", controller, i);
                if (ahci_identify(controller, i) != 0) {
                    print("Identify failed on controller %u port %u\n", controller, i);
                } else {
                    print("Controller %u port %u sectors=%lu sector_size=%u\n",
                             controller, i,
                             (unsigned long)g_drives[controller][i].sector_count,
                             g_drives[controller][i].sector_size);
                }
                break;
            case AHCI_DEV_SATAPI:
                print("SATAPI drive found at controller %u port %u\n", controller, i);
                g_drives[controller][i].controller  = controller;
                g_drives[controller][i].port        = i;
                g_drives[controller][i].device_type = AHCI_DEV_SATAPI;
                g_drives[controller][i].sector_size = AHCI_SECTOR_SIZE;
                g_drives[controller][i].sector_count = 0;
                break;
            case AHCI_DEV_SEMB:
                print("SEMB device at controller %u port %u\n", controller, i);
                g_drives[controller][i].controller  = controller;
                g_drives[controller][i].port        = i;
                g_drives[controller][i].device_type = AHCI_DEV_SEMB;
                break;
            case AHCI_DEV_PM:
                print("Port multiplier at controller %u port %u\n", controller, i);
                g_drives[controller][i].controller  = controller;
                g_drives[controller][i].port        = i;
                g_drives[controller][i].device_type = AHCI_DEV_PM;
                break;
            default:
                print("unknown device type %d controller=%u port=%u\n", type, controller, i);
                break;
        }
    }

    print("port probing done controller=%u\n", controller);
}

static int ahci_submit(uint8_t controller, uint8_t port, uint64_t sector, uint8_t count,
                     void *buf, uint8_t write, ahci_callback_t callback, void *ctx) {
    print("submit controller=%u port=%u sector=%lu count=%u write=%u\n",
             controller, port, (unsigned long)sector, count, write);

    if (count == 0) {
        print("count=0, rejecting\n");
        return -1;
    }

    if (controller >= g_abar_count || g_abars[controller] == NULL) {
        print("invalid controller=%u\n", controller);
        return -1;
    }

    if (port >= AHCI_MAX_PORTS || !g_ahci.controllers[controller].ports[port].present) {
        print("port not present controller=%u port=%u\n", controller, port);
        return -1;
    }

    int prdt_entries = (count + 15) / 16;
    if (prdt_entries > AHCI_MAX_PRDT_ENTRIES) {
        print("too many prdt entries %d\n", prdt_entries);
        return -1;
    }

    HBA_PORT *hba_port = &g_abars[controller]->ports[port];

    asm volatile ("cli");

    int slot = find_cmdslot(hba_port);
    if (slot == -1) {
        asm volatile ("sti");
        print("no free command slot controller=%u port=%u\n", controller, port);
        return -1;
    }

    uint64_t clb_phys = (uint64_t)hba_port->clb | ((uint64_t)hba_port->clbu << 32);
    HBA_CMD_HEADER *cmdheader = (HBA_CMD_HEADER *)phys_to_virt(clb_phys) + slot;

    uint64_t ctba_phys = (uint64_t)cmdheader->ctba | ((uint64_t)cmdheader->ctbau << 32);

    uint32_t saved_ctba  = cmdheader->ctba;
    uint32_t saved_ctbau = cmdheader->ctbau;
    memset(cmdheader, 0, sizeof(*cmdheader));
    cmdheader->ctba  = saved_ctba;
    cmdheader->ctbau = saved_ctbau;
    cmdheader->cfl   = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
    cmdheader->w     = write ? 1 : 0;
    cmdheader->prdtl = (uint16_t)prdt_entries;
    cmdheader->prdbc = 0;

    HBA_CMD_TBL *cmdtbl = (HBA_CMD_TBL *)phys_to_virt(ctba_phys);
    memset(cmdtbl, 0, sizeof(HBA_CMD_TBL) + (prdt_entries - 1) * sizeof(HBA_PRDT_ENTRY));

    uint8_t *buffer   = buf;
    uint8_t remaining = count;
    for (int i = 0; i < prdt_entries; i++) {
        uint32_t entry_sectors = remaining > 16 ? 16 : remaining;
        uint32_t entry_bytes   = entry_sectors * AHCI_SECTOR_SIZE;
        uint64_t buf_phys      = virt_to_phys(buffer);

        cmdtbl->prdt_entry[i].dba  = (uint32_t)(buf_phys & 0xFFFFFFFF);
        cmdtbl->prdt_entry[i].dbau = (uint32_t)(buf_phys >> 32);
        cmdtbl->prdt_entry[i].dbc  = entry_bytes - 1;
        cmdtbl->prdt_entry[i].i    = (i == prdt_entries - 1) ? 1 : 0;

        print("prdt[%d] phys=0x%lx bytes=%u\n", i, (unsigned long)buf_phys, entry_bytes);

        buffer    += entry_bytes;
        remaining -= entry_sectors;
    }

    FIS_REG_H2D *cfis = (FIS_REG_H2D *)&cmdtbl->cfis;
    memset(cfis, 0, sizeof(*cfis));
    cfis->fis_type = FIS_TYPE_REG_H2D;
    cfis->c        = 1;
    cfis->command  = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    cfis->lba0     = (sector >>  0) & 0xFF;
    cfis->lba1     = (sector >>  8) & 0xFF;
    cfis->lba2     = (sector >> 16) & 0xFF;
    cfis->lba3     = (sector >> 24) & 0xFF;
    cfis->lba4     = 0;
    cfis->lba5     = 0;
    cfis->device   = 1 << 6;
    cfis->countl   = count & 0xFF;
    cfis->counth   = (count >> 8) & 0xFF;

    uint32_t spin = 0;
    while ((hba_port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) != 0) {
        if (++spin > AHCI_SPIN_TIMEOUT_ITERS) {
            asm volatile ("sti");
            print("timed out waiting for BSY/DRQ clear, tfd=0x%x\n", hba_port->tfd);
            return -1;
        }
    }

    g_pending[controller][port][slot].callback = callback;
    g_pending[controller][port][slot].ctx = ctx;
    g_pending_mask[controller][port] |= 1u << slot;

    hba_port->is = (uint32_t)-1;
    hba_port->serr = (uint32_t)-1;
    hba_port->ci |= 1u << slot;

    asm volatile ("sti");

    print("submit issued controller=%u port=%u slot=%d ci=0x%x\n",
             controller, port, slot, hba_port->ci);

    return slot;
}

int ahci_read(uint8_t controller, uint8_t port, uint64_t sector, uint8_t count, void *buf,
              ahci_callback_t callback, void *ctx) {
    print("read controller=%u port=%u sector=%lu count=%u\n",
             controller, port, (unsigned long)sector, count);
    return ahci_submit(controller, port, sector, count, buf, 0, callback, ctx);
}

int ahci_write(uint8_t controller, uint8_t port, uint64_t sector, uint8_t count, const void *buf,
               ahci_callback_t callback, void *ctx) {
    print("write controller=%u port=%u sector=%lu count=%u\n",
             controller, port, (unsigned long)sector, count);
    return ahci_submit(controller, port, sector, count, (void *)buf, 1, callback, ctx);
}

static void ahci_handle_port(uint8_t controller, uint8_t port) {
    HBA_PORT *hba_port = &g_abars[controller]->ports[port];

    uint32_t is = hba_port->is;
    if (!is)
        return;

    print("port interrupt controller=%u port=%u is=0x%x\n", controller, port, is);

    uint8_t err = (is & (HBA_PxIS_TFES | HBA_PxIS_HBFS | HBA_PxIS_HBDS | HBA_PxIS_IFS)) != 0;
    if (err) {
        uint32_t tfd = hba_port->tfd;
        uint8_t tfd_status = tfd & 0xFF;
        uint8_t tfd_error  = (tfd >> 8) & 0xFF;

        print("error detected controller=%u port=%u is=0x%x tfd=0x%x status=0x%x error=0x%x\n",
                 controller, port, is, tfd, tfd_status, tfd_error);

        uint64_t fb_phys = (uint64_t)hba_port->fb | ((uint64_t)hba_port->fbu << 32);
        HBA_FIS *fis = (HBA_FIS *)phys_to_virt(fb_phys);
        FIS_REG_D2H *d2h = (FIS_REG_D2H *)fis->rfis;

        print("d2h fis type=0x%x status=0x%x error=0x%x lba0=0x%x lba1=0x%x lba2=0x%x lba3=0x%x lba4=0x%x lba5=0x%x device=0x%x\n",
                 d2h->fis_type, d2h->status, d2h->error,
                 d2h->lba0, d2h->lba1, d2h->lba2, d2h->lba3, d2h->lba4, d2h->lba5, d2h->device);
    }

    uint32_t still_running = hba_port->ci | hba_port->sact;
    uint32_t completed = g_pending_mask[controller][port] & ~still_running;

    if (err) {
        completed |= g_pending_mask[controller][port];
    }

    print("pending=0x%x still_running=0x%x completed=0x%x\n",
             g_pending_mask[controller][port], still_running, completed);

    for (uint8_t slot = 0; slot < AHCI_MAX_CMD_SLOTS; slot++) {
        if (!(completed & (1u << slot)))
            continue;

        ahci_callback_t callback = g_pending[controller][port][slot].callback;
        void *ctx = g_pending[controller][port][slot].ctx;

        g_pending_mask[controller][port] &= ~(1u << slot);
        g_pending[controller][port][slot].callback = NULL;
        g_pending[controller][port][slot].ctx = NULL;

        print("completing slot=%u err=%u callback=%X\n", slot, err, (void *)callback);

        if (callback)
            callback(controller, port, slot, err, ctx);
    }

    hba_port->is = is;
    hba_port->serr = (uint32_t)-1;

    if (err) {
        stop_cmd(hba_port);
        start_cmd(hba_port);
        hba_port->ci = 0;
        hba_port->sact = 0;
        g_pending_mask[controller][port] = 0;
        print("recovered command engine controller=%u port=%u\n", controller, port);
    }
}

uint8_t ahci_init() {
    print("init start\n");

    memset(&g_ahci, 0, sizeof(g_ahci));
    memset(g_drives, 0, sizeof(g_drives));
    memset(g_pending, 0, sizeof(g_pending));
    memset(g_pending_mask, 0, sizeof(g_pending_mask));

    for (int i = 0; i < AHCI_MAX_CONTROLLERS; i++)
        g_abars[i] = NULL;

    g_abar_count = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint8_t func_limit = 1;

            for (uint8_t func = 0; func < func_limit; func++) {
                uint16_t vendor = pci_config_read_word(bus, slot, func, 0);
                if (vendor == 0xffff)
                    continue;

                if (func == 0) {
                    uint8_t header_type = pci_config_read_byte(bus, slot, func, 0x0e);
                    if (header_type & 0x80)
                        func_limit = 8;
                }

                if (!pci_is_ahci_device(bus, slot, func))
                    continue;

                print("found controller at %u:%u:%u vendor=0x%x\n", bus, slot, func, vendor);

                pci_enable_device(bus, slot, func);

                uint64_t abar_phys = pci_read_bar64(bus, slot, func, 0x24);
                if (abar_phys == 0) {
                    print("abar_phys is 0 at %u:%u:%u, skipping\n", bus, slot, func);
                    continue;
                }

                if (g_abar_count >= AHCI_MAX_CONTROLLERS) {
                    print("Max controllers reached, skipping %u:%u:%u\n",
                             bus, slot, func);
                    continue;
                }

                uint64_t abar_virt = (uint64_t)phys_to_virt((uintptr_t)abar_phys);

                uint64_t *pml4 = (uint64_t *)phys_to_virt((uintptr_t)kernel_pml4);

                map_page(pml4, (void *)abar_virt, abar_phys,
                                  PAGE_WRITABLE);

                map_page(pml4, phys_to_virt(AHCI_BASE), AHCI_BASE,
                                 PAGE_WRITABLE);

                HBA_MEM *abar = (HBA_MEM *)abar_virt;

                ahci_bios_handoff(abar);

                if (ahci_hba_enable(abar) != 0) {
                    print("Controller at %u:%u:%u failed to enable, skipping\n",
                             bus, slot, func);
                    continue;
                }

                uint8_t ctrl_idx = g_abar_count;

                g_abars[ctrl_idx] = abar;
                g_ahci.controllers[ctrl_idx].abar = abar;
                g_abar_count++;

                print("Controller %u found at %u:%u:%u base 0x%lx\n",
                         ctrl_idx, bus, slot, func, (unsigned long)abar_phys);

                ahci_probe_ports(abar, ctrl_idx);

                uint8_t irq = pci_get_interrupt_line(bus, slot, func);
                print("Controller %u irq line=%u\n", ctrl_idx, irq);
                ioapic_set_entry(irq, 0x21);
                ioapic_unmask(irq);

                abar->ghc |= AHCI_GHC_IE;
            }
        }
    }

    g_ahci.count = g_abar_count;
    print("init done, controller_count=%u\n", g_abar_count);
    return 0;
}

uint8_t ahci_get_controller_count() {
    return g_ahci.count;
}

ahci_controller_t *ahci_get_controller(uint8_t index) {
    if (index >= g_ahci.count)
        return NULL;
    return &g_ahci.controllers[index];
}

uint8_t ahci_get_port_count(uint8_t controller) {
    if (controller >= g_ahci.count)
        return 0;

    uint8_t count = 0;
    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (g_ahci.controllers[controller].ports[i].present)
            count++;
    }
    return count;
}

uint8_t ahci_get_port_index(uint8_t controller, uint8_t n) {
    if (controller >= g_ahci.count)
        return 0xFF;

    uint8_t found = 0;
    for (uint8_t i = 0; i < AHCI_MAX_PORTS; i++) {
        if (g_ahci.controllers[controller].ports[i].present) {
            if (found == n)
                return i;
            found++;
        }
    }
    return 0xFF;
}

drive_t *ahci_get_drive(uint8_t controller, uint8_t port) {
    if (controller >= g_abar_count || port >= AHCI_MAX_PORTS)
        return NULL;
    if (!g_ahci.controllers[controller].ports[port].present)
        return NULL;
    return &g_drives[controller][port];
}

void ahci_handler() {
    apic_eoi();

    print("interrupt handler entered\n");

    for (uint8_t c = 0; c < g_abar_count; c++) {
        HBA_MEM *abar = g_abars[c];
        if (!abar)
            continue;

        uint32_t pending_ports = abar->is;
        if (!pending_ports)
            continue;

        print("controller=%u pending_ports=0x%x\n", c, pending_ports);

        for (uint8_t p = 0; p < AHCI_MAX_PORTS; p++) {
            if (pending_ports & (1u << p))
                ahci_handle_port(c, p);
        }

        abar->is = pending_ports;
    }
}