#pragma once

#include <stdint.h>

typedef struct {
    uint8_t controller;
    uint8_t port;
} ahci_blockdev_priv_t;