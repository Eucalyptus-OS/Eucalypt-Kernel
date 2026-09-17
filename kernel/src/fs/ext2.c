#include <stdint.h>

struct super_block {
    uint32_t inode_count;
    uint32_t block_count;
    uint32_t super_user_blocks;
    uint32_t unallocatez_count;
    uint32_t unallocated_inode_count;
    uint32_t superblock_block;
    uint32_t block_size;
    uint32_t fragment_size;
    
} __attribute ((packed));
