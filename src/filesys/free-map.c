#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"

static struct bitmap *free_map;    /* Free map, one bit per sector. */
static block_sector_t free_map_begin;
static block_sector_t free_map_end;
static block_sector_t root_dir_sector;
static struct lock free_map_lock;

static bool 
free_map_write (void)
{
  return bitmap_write (free_map, free_map_begin);
}

static bool
free_map_read (void)
{
  return bitmap_read (free_map, free_map_begin);
}

block_sector_t free_map_root_sector ()
{
  return root_dir_sector;
}

/* Initializes the free map. */
void
free_map_init (void) 
{
  free_map = bitmap_create (block_size (fs_device));
  if (free_map == NULL)
    PANIC ("bitmap creation failed--file system device is too large");

  /* Set all of the bits that belong to the free map */
  size_t num_sectors = bitmap_sector_size (free_map);

  /* Internally store the range of the free map on disk */
  free_map_begin = FREE_MAP_SECTOR_BEGIN;
  free_map_end = free_map_begin + num_sectors;

  block_sector_t i;
  for (i = free_map_begin; i < free_map_end; i++)
    bitmap_mark (free_map, i);

  /* Set the bit that belongs to the root inode */
  root_dir_sector = free_map_end;
  bitmap_mark (free_map, root_dir_sector);

  /* Set the cwd for the main thread */
  thread_set_cwd (root_dir_sector);

  lock_init (&free_map_lock);
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available or if the free_map file could not be
   written. */
bool
free_map_allocate (size_t cnt, block_sector_t *sectorp)
{
  lock_acquire (&free_map_lock);
  block_sector_t sector = bitmap_scan_and_flip (free_map, 0, cnt, false);
  if (sector != BITMAP_ERROR
      && !free_map_write ())
  {
    bitmap_set_multiple (free_map, sector, cnt, false); 
    sector = BITMAP_ERROR;
  }
  lock_release (&free_map_lock);

  if (sector != BITMAP_ERROR)
    *sectorp = sector;
  return sector != BITMAP_ERROR;
}

/* Makes CNT sectors starting at SECTOR available for use. */
void
free_map_release (block_sector_t sector, size_t cnt)
{
  ASSERT (bitmap_all (free_map, sector, cnt));
  lock_acquire (&free_map_lock);

  bitmap_set_multiple (free_map, sector, cnt, false);
  free_map_write ();

  lock_release (&free_map_lock);
}

/* Opens the free map file and reads it from disk. */
void
free_map_open (void) 
{
  lock_acquire (&free_map_lock);

  if (!free_map_read ())
    PANIC ("can't read free map");

  lock_release (&free_map_lock);
}

/* Writes the free map to disk and closes the free map file. */
void
free_map_close (void) 
{
  lock_acquire (&free_map_lock);
  free_map_write ();
  lock_release (&free_map_lock);
}

/* Creates a new free map file on disk and writes the free map to
   it. */
void
free_map_create (void) 
{
  lock_acquire (&free_map_lock);

  /* Write bitmap to file. */
  if (!free_map_write ())
    PANIC ("can't write free map");

  lock_release (&free_map_lock);
}
