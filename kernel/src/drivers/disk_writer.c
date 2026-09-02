#include <drivers/disk_writer.h>
#include <logging/print.h>
#include <drivers/ahci.h>
#include <drivers/drive_map.h>
#include <multitasking/thread.h>
#include <mm/heap.h>
#include <multitasking/sched.h>
#include <stdint.h>

struct pending_io {
    struct tcb *tcb;
    uint8_t status;
    uint8_t done;
};

static void disk_complete(uint8_t controller, uint8_t port, uint8_t slot, uint8_t status, void *ctx) {
    print("Disk completed\nController: %d\nPort: %d\nSlot: %d\n", controller, port, slot);
    struct pending_io *io = ctx;
    io->status = status;
    io->done = 1;
    unblock(io->tcb);
}

uint8_t disk_writer(uint8_t drive_number, uint64_t sector, uint8_t count, const void *data) {
    uint8_t controller, port;
    if (drive_map_resolve(drive_number, &controller, &port) != 0) {
        print("disk_writer(): invalid drive number %u\n", drive_number);
        return 1;
    }

    print("Writing to disk\nDrive: %d\nController: %d\nPort: %d\n", drive_number, controller, port);

    struct pending_io *io = kmalloc(sizeof(struct pending_io));
    if (!io) {
        print("Failed to allocate pending_io\n");
        return 1;
    }

    io->tcb = get_current_thread();
    io->status = 0;
    io->done = 0;

    if (!io->tcb) {
        print("Failed to get current tcb\n");
        kfree(io);
        return 1;
    }

    asm volatile ("cli");

    int slot = ahci_write(controller, port, sector, count, data, disk_complete, io);
    if (slot < 0) {
        asm volatile ("sti");
        print("Failed to write to disk\nDrive: %d\nController: %d\nPort: %d\n", drive_number, controller, port);
        kfree(io);
        return 1;
    }

    asm volatile ("sti");

    print("Wrote to disk\nDrive: %d\nController: %d\nPort: %d\nSuccessfully\n", drive_number, controller, port);

    while (!io->done) {
        block_current();
    }

    uint8_t status = io->status;
    kfree(io);
    return status;
}

uint8_t disk_reader(uint8_t drive_number, uint64_t sector, uint8_t count, void *data) {
    uint8_t controller, port;
    if (drive_map_resolve(drive_number, &controller, &port) != 0) {
        print("disk_reader(): invalid drive number %u\n", drive_number);
        return 1;
    }

    print("Reading from disk\nDrive: %d\nController: %d\nPort: %d\n", drive_number, controller, port);

    struct pending_io *io = kmalloc(sizeof(struct pending_io));
    if (!io) {
        print("Failed to allocate pending_io\n");
        return 1;
    }

    io->tcb = get_current_thread();
    io->status = 0;
    io->done = 0;

    if (!io->tcb) {
        print("Failed to get current tcb\n");
        kfree(io);
        return 1;
    }

    asm volatile ("cli");

    int slot = ahci_read(controller, port, sector, count, data, disk_complete, io);
    if (slot < 0) {
        asm volatile ("sti");
        print("Failed to read disk\nDrive: %d\nController: %d\nPort: %d\n", drive_number, controller, port);
        kfree(io);
        return 1;
    }

    asm volatile ("sti");

    while (!io->done) {
        block_current();
    }

    uint8_t status = io->status;
    kfree(io);
    print("Disk read\nDrive: %d\nController: %d\nPort: %d\n", drive_number, controller, port);
    return status;
}