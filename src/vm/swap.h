#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/block.h"

bool swap_load (uint8_t *dest, block_sector_t swap_block);
block_sector_t swap_write (uint8_t *src);

#endif /* vm/swap.h */
