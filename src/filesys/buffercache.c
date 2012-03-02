#include <debug.h>
#include <string.h>

#include "devices/block.h"
#include "filesys/buffercache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/palloc.h"

static struct cache_entry *cache; /* Cache entry table */
static struct lock cache_lock;	  /* Lock for entry table */
static int cache_size;		  /* Size of the cache */
static int clock_hand;		  /* For clock algorithm */

static bool buffercache_allocate_block (struct cache_entry *entry);
static struct cache_entry *buffercache_find_entry (const block_sector_t sector);
static struct cache_entry *buffercache_evict (void);
static void buffercache_read_ahead_if_necessary (const block_sector_t sector);
static void buffercache_flush_entry (struct cache_entry *entry);
static struct cache_entry *clock_algorithm (void);
static inline int clock_next (void);

/**
 * Initializes the buffer cache system. Returns true on success, false on
 * error.
 */
bool
buffercache_init (const size_t size)
{
  int i;
  bool result;

  /* Set the cache size */
  cache_size = size;

  /* Initialize list of pages */
  cache = malloc (cache_size * sizeof (struct cache_entry));
  if (cache == NULL) return false;
  lock_init (&cache_lock);

  /* Allocate the cache pages */
  for (i = 0; i < cache_size; i++)
  {
    result = buffercache_allocate_block (&cache[i]);
    if (!result) return false;
  }

  /* Initialize the clock hand so first access will be slot 0 */
  clock_hand = cache_size - 1;

  return true;
}

/**
 * Reads a sector from sector into buf. Does not do bounds checking on
 * sector_ofs and size.
 *
 * Returns the number of bytes read, or -1 on failure.
 */
int
buffercache_read (const block_sector_t sector, const int sector_ofs,
		  const off_t size, void *buf)
{
  struct cache_entry *entry;

  /* Finds an entry and returns it locked */
  entry = buffercache_find_entry (sector);
  if (entry == NULL)
    entry = buffercache_evict ();

  if (entry != NULL)
  {
    /* Read from the cache entry */
    ASSERT (lock_held_by_current_thread (&entry->l));

    entry->accessed |= ACCESSED;
    memcpy ((void*)buf, (void*)entry->kaddr, BLOCK_SECTOR_SIZE);

    lock_release (&entry->l);
  } else {
    /* Failsafe: bypass the cache */
    /* TODO: use bounce buffer */
    block_read (fs_device, sector, buf);
  }

  buffercache_read_ahead_if_necessary (sector);

  return size;
}

/**
 * Writes a sector from buf into sector. Does not do bounds checking on
 * sector_ofs and size.
 * 
 * Returns the number of bytes written, or -1 on failure.
 */
int
buffercache_write (const block_sector_t sector, const int sector_ofs,
		   const off_t size, const void *buf)
{
  struct cache_entry *entry;

  /* Finds an entry and returns it locked */
  entry = buffercache_find_entry (sector);
  if (entry == NULL)
    entry = buffercache_evict ();

  if (entry != NULL)
  {
    /* Write to cache entry */
    ASSERT (lock_held_by_current_thread (&entry->l));

    entry->accessed |= ACCESSED | DIRTY;
    memcpy ((void *)entry->kaddr + sector_ofs, (void *)buf, size);

    lock_release (&entry->l);
  } else {
    /* Failsafe: bypass the cache */
    /* TODO: use bounce buffer */
    block_write (fs_device, sector, buf);
  }

  buffercache_read_ahead_if_necessary (sector);

  return size;
}

/**
 * Flushes all dirty buffers in the cache to disk.
 */
void
buffercache_flush (void)
{
  int i;

  lock_acquire (&cache_lock);

  for (i = 0; i < cache_size; i++)
    buffercache_flush_entry (&cache[i]);

  lock_release (&cache_lock);
}

/**
 * Initializes an entry in the buffer cache table.
 */
static bool
buffercache_allocate_block (struct cache_entry *entry)
{
  entry->kaddr = (uint32_t)palloc_get_page (0);
  if ((void *)entry->kaddr == NULL) return false;

  entry->sector = -1;
  entry->state = READY;
  entry->accessed = CLEAN;

  lock_init (&entry->l);
  cond_init (&entry->c);

  return true;
}

/**
 * Flushes the specified cache entry to disk if necssary.
 */
static void
buffercache_flush_entry (struct cache_entry *entry)
{
  lock_acquire (&entry->l);

  if (entry->accessed & DIRTY)
  {
    /* Write to disk */
  }

  entry->accessed |= CLEAN;

  lock_release (&entry->l);
}

/**
 * Invokes a read-ahead thread for the given cache entry if needed.
 */
static void
buffercache_read_ahead_if_necessary (const block_sector_t sector)
{
  /* TODO implement pre-fetch */
  return;
}

/**
 * Returns the cache entry number for the given sector if it is cached,
 * else -1. Acquires the cache lock.
 *
 * Returns the entry locked, or NULL if no entry could be evicted.
 */
static struct cache_entry *
buffercache_find_entry (const block_sector_t sector)
{
  int i;

  lock_acquire (&cache_lock);

  for (i = 0; i < cache_size; i++)
  {
    if (cache[i].sector == sector)
    {
      lock_acquire (&cache[i].l);
      lock_release (&cache_lock);
      return &cache[i];
    }
  }

  lock_release (&cache_lock);

  return NULL;
}

/**
 * Use the clock algorithm to find an entry to evict and flush it to
 * disk.
 *
 * Returns the entry locked, or NULL if no entry could be evicted.
 */
static struct cache_entry *
buffercache_evict (void)
{
  struct cache_entry *e;

  ASSERT (lock_held_by_current_thread (&cache_lock));

  e = clock_algorithm ();
  if (e == NULL) return NULL;

  buffercache_flush_entry (e);
  return e;
}

/**
 * Runs the clock algorithm to find the next entry to evict. The cache
 * lock must be held when calling this.
 *
 * The algorithm proceeds as follows: for each advancement of the clock,
 * if the entry is locked it is ignored. If the accessed bit is set it is
 * reset. If the accessed bit is not set then this entry is returned. If
 * we wrap around then the clock hand is returned.
 *
 * Returns a locked entry that is ready to be flushed to disk and replaced.
 */
static struct cache_entry *
clock_algorithm (void)
{
  int clock_start;
  struct cache_entry *e;
  bool acquired;

  ASSERT (lock_held_by_current_thread (&cache_lock));

  clock_start = clock_next ();
  e = &cache[clock_start];

  do
  {
    /* If lock is already acquired, ignore this entry */
    acquired = lock_try_acquire (&e->l);
    if (!acquired) continue;

    if (e->state == WRITING)
    {
      /* Some I/O is happening to this block, ignore */
      lock_release (&e->l);
      e = &cache[clock_next ()];
    } else if (e->accessed & ACCESSED) {
      /* Access bit is set, unset it and continue */
      e->accessed &= ~ACCESSED;
      lock_release (&e->l);
      e = &cache[clock_next ()];
    } else {
      /* Access bit is not set, return this entry (locked) */
      return e;
    }

  } while (clock_hand != clock_start);

  return e;
}

/**
 * Helper function for the clock algorithm which treats the entries as a
 * circularly linked list.
 */
static inline int
clock_next (void)
{
  return (clock_hand = (clock_hand + 1) % cache_size);
}
