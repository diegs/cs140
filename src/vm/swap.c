#include "vm/frame.h"
#include "vm/swap.h"

/**
 * Loads a swap block into uaddr. Returns true on success.
 */
bool
swap_load (uint8_t *dest, block_sector_t swap_block)
{
  return true;
}

/**
 * Writes a page from uaddr into the swap partition. Returns the swap
 * block used.
 */
block_sector_t
swap_write (uint8_t *src)
{
  return 0;
}
