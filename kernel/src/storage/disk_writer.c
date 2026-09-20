#include <stdint.h>
#include <logging/print.h>
#include <storage/ahci.h>
#include <storage/drive_map.h>
#include <multitasking/thread.h>
#include <mm/heap.h>
#include <multitasking/sched.h>
#include <storage/disk_writer.h>

// Completion context for one synchronous I/O: the AHCI callback wakes the waiting thread via tcb
struct pending_io {
    struct tcb *tcb;
    uint8_t status;   // disk completion status (0 = success)
    uint8_t done;     // set by the callback once the I/O has finished
};

// AHCI async callback: record the transfer result and unblock the thread that issued the request
static void disk_complete(uint8_t controller, uint8_t port, uint8_t slot, uint8_t status, void *ctx) {
    (void)controller;
    (void)port;
    (void)slot;
    struct pending_io *io = ctx;
    io->status = status;
    io->done = 1;
    unblock(io->tcb);
}

// Write `count` sectors at `sector` to `drive_number`; blocks until completion, returns disk status
uint8_t disk_writer(uint8_t drive_number, uint64_t sector, uint8_t count, const void *data) {
    uint8_t controller, port;
    if (drive_map_resolve(drive_number, &controller, &port) != 0) {
        print("disk_writer(): invalid drive number %u\n", drive_number);
        return 1;
    }

    struct pending_io *io = kmalloc(sizeof(struct pending_io));
    if (!io) {
        print("Failed to allocate pending_io\n");
        return 1;
    }

    io->tcb = sched_current_thread();
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

    // Park the thread with IRQs masked until the completion callback marks io->done
    for (;;) {
        asm volatile ("cli");
        if (io->done)
            break;
        block_current();
    }
    asm volatile ("sti");

    uint8_t status = io->status;
    kfree(io);
    return status;
}

// Read `count` sectors at `sector` from `drive_number` into `data`; blocks until completion, returns disk status
uint8_t disk_reader(uint8_t drive_number, uint64_t sector, uint8_t count, void *data) {
    uint8_t controller, port;
    if (drive_map_resolve(drive_number, &controller, &port) != 0) {
        print("disk_reader(): invalid drive number %u\n", drive_number);
        return 1;
    }

    struct pending_io *io = kmalloc(sizeof(struct pending_io));
    if (!io) {
        print("Failed to allocate pending_io\n");
        return 1;
    }

    io->tcb = sched_current_thread();
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

    // Park the thread with IRQs masked until the completion callback marks io->done
    for (;;) {
        asm volatile ("cli");
        if (io->done)
            break;
        block_current();
    }
    asm volatile ("sti");

    uint8_t status = io->status;
    kfree(io);
    return status;
}