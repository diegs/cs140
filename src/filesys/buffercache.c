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
static int buffercache_find_entry (const block_sector_t sector);
static int buffercache_evict (void);
static void buffercache_read_ahead_if_necessary (const block_sector_t sector);
static void buffercache_flush_entry (struct cache_entry *entry);
static int clock_algorithm (void);
static int clock_next (void);

/**
 * Initializes the buffer cache system
 */
bool
buffercache_init (const size_t size)
{
  int i;
  bool result;

  cache_size = size;

  /* Initialize list of pages */
  cache = malloc (cache_size * sizeof (struct cache_entry));
  if (cache == NULL) return false;
  lock_init (&cache_lock);

  /* Allocate the cache pages */
  for (i=0; i<cache_size; i++)
  {
    result = buffercache_allocate_block (&cache[i]);
    if (!result) return false;
  }

  /* Initialize the clock hand so first access will be slot 0 */
  clock_hand = cache_size - 1;

  return true;
}

/**
 * Reads a sector from sector into buf. Returns the number of bytes
 * read, or -1 on failure.
 */
int
buffercache_read (const block_sector_t sector, const int sector_ofs,
		  const off_t size, void *buf)
{
  struct cache_entry *entry;

  /* Searches for an existing entry and returns it locked */
  entry = buffercache_find_entry (sector);	

  /* Cache miss */
  if (entry == NULL)
  {
	/* Try to free a cache entry */
	entry = buffercache_evict();

	/* If that failed too, read it without caching */
	if (entry == NULL)
	{
	  block_read (fs_device, sector, buf);
	  return size;
	}
	/* Read the file block into our cache entry */
	block_read (fs_device, sector, entry->kaddr);
	entry->accessed |= ACCESSED;
  }
  /* Copy it from cache into buf*/    
  memcpy ((void *)buf, (void *)entry->kaddr + sector_ofs, size);
  lock_release (&entry->l);
  buffercache_read_ahead_if_necessary (sector);
  return size;
}

/**
 * Writes a sector from buf into sector. Returns the number of bytes
 * written, or -1 on failure.
 */
int
buffercache_write (const block_sector_t sector, const int sector_ofs,
		   const off_t size, const void *buf)
{

  struct cache_entry *entry;

  /* Searches for an existing entry and returns it locked */
  entry = buffercache_find_entry (sector);	

  /* Cache miss */
  if (entry == NULL)
  {
	/* Try to free a cache entry */
	entry = buffercache_evict();

	/* If that failed too, read it without caching */
	if (entry == NULL)
	{
	  block_write (fs_device, sector, buf);
	  return size;
	}
	/* Read the file block into our cache entry */
	block_read (fs_device, sector, entry->kaddr);
	entry->accessed |= ACCESSED;
  }
	
  /* Write to the cache */
  memcpy ((void *)entry->kaddr + sector_ofs, (void *)buf, size);
  entry->accessed |= DIRTY;
  lock_release (&entry->l);

  return size;
}

/**
 * Flushes all dirty buffers in the cache to disk
 */
bool
buffercache_flush (void)
{
  int i;

  lock_acquire (&cache_lock);
  for (i=0; i<cache_size; i++)
  {
    if (cache[i].accessed & DIRTY)
    {
      lock_acquire (&cache[i].l);
      buffercache_flush_entry (&cache[i]);
      lock_release (&cache[i].l);
    }
  }

  lock_release (&cache_lock);

  return true;
}

/**
 * Flushes the specified cache entry to disk. Assumes the entry's lock is
 * already held.
 */
static void
buffercache_flush_entry (struct cache_entry *entry)
{
  ASSERT(lock_held_by_current_thread (&entry->l));
  if (entry->accessed & DIRTY)
  {
	//write it
    block_write (fs_device, entry->sector, entry->kaddr);	
  }
	
  /* Reset entry to blank state */
  entry->kaddr = NULL;
  entry->state = READY;
  entry->accessed = CLEAN;
  entry->sector = 0;
}


/**
 * Initializes an entry in the buffer cache table
 */
static bool
buffercache_allocate_block (struct cache_entry *entry)
{
  entry->kaddr = (uint32_t)palloc_get_page (0);
  if ((void *)entry->kaddr == NULL) return false;

  entry->state = READY;
  entry->accessed = CLEAN;
  entry->sector = 0;
  lock_init (&entry->l);
  cond_init (&entry->c);

  return true;
}


/**
 * Invokes a read-ahead thread for the given cache entry if needed
 */
static void
buffercache_read_ahead_if_necessary (const block_sector_t sector)
{
  /* TODO Invoke pre-fetch */
  return;
}

/* Looks for an entry  */
static struct cache_entry *
buffercache_find_entry (int sector)
{
  lock_acquire(&cache_lock);
  struct cache_entry * e;
  int i;
  for (i = 0; i < cache_size; i++)
  {
	e = &cache[i];
	if (e->sector == sector)
	{
	  /* Acquire the entry's lock and return it */
	  lock_acquire(&e->l);
	  lock_release(&cache_lock);
	  return e;
	}
  }
  lock_release(&cache_lock);
  return NULL;
}

/* Use the clock algorithm to find an entry to evict and flush it to
   disk */
static struct cache_entry * buffercache_evict()
{
  lock_acquire(&cache_lock);
  int evicted = clock_algorithm (); 
  if (evicted == -1) return NULL;
  struct cache_entry * e = &cache[evicted];
  lock_acquire(&e->l);
  buffercache_flush_entry(e);
  lock_release(&cache_lock);
  return e;
}

/* Searches for an open entry in the buffer cache */
int buffercache_find_open_entry()
{
  int i;
  for (i = 0; i < cache_size; i++)
  {
	if (cache[i].kaddr == NULL) return i;
  }
  return -1;
}

/**
 * Runs the clock algorithm to find the next entry to evict.  The cache
 * lock should be held when calling this.  Returns -1 if it couldn't find
 * an entry to free
 */
static int
clock_algorithm (void)
{
  ASSERT(lock_held_by_current_thread (&cache_lock));

  int clock_start = clock_next ();
  struct cache_entry * e = &cache[clock_start];
	
  while (e->state == WRITING)
  {	
    e = &cache[clock_next ()];
    /* Check that we haven't looped all the way around */
    if (clock_hand == clock_start) return -1;
  }
  return clock_hand;
}

/**
 * Helper function for the clock algorithm which treats the entries as a
 * circularly linked list
 */
static int 
clock_next (void)
{
  clock_hand = (clock_hand + 1) % cache_size;
  return clock_hand;
}
