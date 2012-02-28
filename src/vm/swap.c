#include <bitmap.h>
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "vm/frame.h"
#include "vm/swap.h"

#define BLOCKS_PER_PAGE PGSIZE/BLOCK_SECTOR_SIZE

struct bitmap *swap_table;	/* Directory of free/used swap blocks */
struct lock swap_lock;		/* Protects swap_table */

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
  struct block *swap;
  size_t size;

  /* Gracefully handle absence of swap */
  swap = get_swap ();
  if (swap == NULL)
    size = 0;
  else
    size = block_size (swap);

  swap_table = bitmap_create (size);
  if (swap_table == NULL)
    PANIC ("Could not initialize swap");
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
swap_load (uint8_t *dest, block_sector_t swap_begin)
{
  int i;

  lock_acquire (&swap_lock);
  bitmap_set_multiple (swap_table, swap_begin, BLOCKS_PER_PAGE, false);
  lock_release (&swap_lock);

  for (i = 0; i < BLOCKS_PER_PAGE; i++)
  {
    block_read (get_swap (), i + swap_begin, dest +
                  i*BLOCK_SECTOR_SIZE);
  }

  return true;
}

/**
 * Writes a page from uaddr into the swap partition. Returns the swap
 * block used.
 */
bool
swap_write (uint8_t *src, block_sector_t *swap_out)
{
  int i;

  lock_acquire (&swap_lock);
  block_sector_t swap_begin = bitmap_scan_and_flip (swap_table, 
						    0, BLOCKS_PER_PAGE, false);
  if (swap_begin == BITMAP_ERROR)
  {
    lock_release (&swap_lock);
    PANIC ("Out of swap!");
  }
  lock_release (&swap_lock);

  for (i = 0; i < BLOCKS_PER_PAGE; i++)
  {
    block_write (get_swap (), i + swap_begin, src +
		 i*BLOCK_SECTOR_SIZE);
  }

  *swap_out = swap_begin;

  return true;
}

/**
 * Frees the swap blocks starting at the designated sector.
 */
void
swap_free (block_sector_t swap_begin)
{
  lock_acquire (&swap_lock);
  bitmap_set_multiple (swap_table, swap_begin, BLOCKS_PER_PAGE, false);
  lock_release (&swap_lock);
}
