#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/block.h"

void swap_init (void);
void swap_destroy (void);
bool swap_load (uint8_t *dest, block_sector_t swap_blocks[]);
bool swap_write (uint8_t *src, block_sector_t swap_blocks[]);

#endif /* vm/swap.h */
