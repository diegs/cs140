#include <bitmap.h>
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "vm/frame.h"
#include "vm/swap.h"

struct bitmap *swap_table;
struct lock swap_lock;

static inline struct block *
get_swap (void)
{
  return block_get_role (BLOCK_SWAP);
}

/**
 * Initializes the swap table
 */
void
swap_init (void)
{
  swap_table = bitmap_create (block_size (get_swap ()));
  lock_init (&swap_lock);
}

/**
 * Destroys the swap table (never actually called)
 */
void
swap_destroy (void)
{
  bitmap_destroy (swap_table);
}

/**
 * Loads a swap block into uaddr. Returns true on success.
 */
bool
swap_load (uint8_t *dest, block_sector_t swap_blocks[])
{
  int i;
  lock_acquire (&swap_lock);
  for (i=0; i<PGSIZE/BLOCK_SECTOR_SIZE; i++)
  {
    bitmap_reset (swap_table, swap_blocks[i]);
  }
  lock_release (&swap_lock);

  for (i=0; i<PGSIZE/BLOCK_SECTOR_SIZE; i++)
  {
    block_read (get_swap (), swap_blocks[i], dest + i*BLOCK_SECTOR_SIZE);
  }

  return true;
}

/**
 * Writes a page from uaddr into the swap partition. Returns the swap
 * block used.
 */
bool
swap_write (uint8_t *src, block_sector_t swap_blocks[])
{
  int i;
  lock_acquire (&swap_lock);
  for (i=0; i<PGSIZE/BLOCK_SECTOR_SIZE; i++)
  {
    swap_blocks[i] = bitmap_scan_and_flip (swap_table, 0, 1, false);
    if (swap_blocks[i] == BITMAP_ERROR)
    {
      lock_release (&swap_lock);
      PANIC ("Out of swap!");
    }
  }
  lock_release (&swap_lock);

  for (i=0; i<PGSIZE/BLOCK_SECTOR_SIZE; i++)
  {
    block_write (get_swap (), swap_blocks[i], src + i*BLOCK_SECTOR_SIZE);
  }

  return true;
}
