#pragma once

#include <stddef.h>
#include <stdint.h>

// Shared-memory regions: the userspace-visible half of /dev/shm.
//
// A region is a physically-backed run of pages. Any process can map one with
// MAP_SHARED at an offset of (index * PAGE_SIZE) and every mapper sees the same
// physical bytes. The compositor allocates one region per window so a client
// draws straight into its own backing store -- no copy on the way in, and no
// way to scribble on a neighbour's pixels.

// Number of regions the kernel tracks, and the longest region name
#define SHM_MAX_REGIONS 64
#define SHM_NAME_LEN    32

// ioctl requests accepted by /dev/shm
#define SHM_IOC_CREATE  0x4680  // arg: shm_create_t*
#define SHM_IOC_DESTROY 0x4681  // arg: char[SHM_NAME_LEN]
#define SHM_IOC_QUERY   0x4682  // arg: shm_query_t*

// Create request. On success `index` receives the region index, which is also
// the mmap offset that selects it.
typedef struct {
    char     name[SHM_NAME_LEN];
    size_t   size;
    uint64_t index;      // out
} shm_create_t;

// Query reply: lets a client find a region somebody else created
typedef struct {
    char     name[SHM_NAME_LEN];
    size_t   size;
    uint64_t index;      // out
    int      found;      // out
} shm_query_t;
