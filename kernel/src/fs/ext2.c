#include <stdint.h>

// On-disk ext2 superblock layout (first part of block group 0); stub for future support
struct super_block {
    uint32_t inode_count;               // total inodes across the filesystem
    uint32_t block_count;               // total blocks across the filesystem
    uint32_t super_user_blocks;         // blocks reserved for the super user
    uint32_t unallocatez_count;         // free / unallocated blocks
    uint32_t unallocated_inode_count;   // free / unallocated inodes
    uint32_t superblock_block;          // block number holding this superblock
    uint32_t block_size;                // stored as log2(size) - 10
    uint32_t fragment_size;             // stored as log2(size) - 10
    // (remaining superblock fields would follow here)
} __attribute ((packed));
