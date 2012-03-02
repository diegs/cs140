#include <debug.h>
#include "filesys/buffercache.h"
#include "filesys/filesys.h"
#include "devices/block.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "lib/string.h"


static struct cache_entry *cache; /* Cache entry table */
static struct lock cache_lock;	  /* Lock for entry table */
static int cache_size;		  /* Size of the cache */
static int clock_hand;		  /* For clock algorithm */

static bool buffercache_allocate_block (struct cache_entry *entry);
static int buffercache_find_entry (int sector);
static int buffercache_evict(void);
static void buffercache_read_ahead_if_necessary(struct cache_entry *entry);
static void buffercache_flush_entry (struct cache_entry *entry);
static int clock_next(void);

/**
 * Initializes the buffer cache system
 */
bool
buffercache_init (size_t size)
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
buffercache_read (int sector, void *buf)
{
  int entry_num;
  struct cache_entry *entry;

  /* Finds an entry and returns it locked */
  entry_num = buffercache_find_entry (sector);
  if (entry_num == -1)
    entry_num = buffercache_evict ();

  if (entry_num != -1) 
  {
    /* Read from the cache entry */
    entry = &cache[entry_num];
    buffercache_read_ahead_if_necessary (entry);
    entry->accessed |= ACCESSED;
    
    memcpy ((void *)buf, (void *)entry->kaddr, BLOCK_SECTOR_SIZE);
    lock_release (&entry->l);
  } else {
    /* Failsafe: bypass the cache */
    block_read (fs_device, sector, buf);
  }

  return BLOCK_SECTOR_SIZE;
}

/**
 * Writes a sector from buf into sector. Returns the number of bytes
 * written, or -1 on failure.
 */
int
buffercache_write (int sector, const void *buf)
{
  int entry_num;
  struct cache_entry *entry;

  /* Finds an entry and returns it locked */
  entry_num = buffercache_find_entry (sector);
  if (entry_num == -1)
    entry_num = buffercache_evict ();

  if (entry_num != -1) 
  {
    /* Read from the cache entry */
    entry = &cache[entry_num];
    memcpy ((void *)entry->kaddr, (void *)buf, BLOCK_SECTOR_SIZE);
    entry->accessed |= DIRTY;
    /* TODO: invoke read-ahead if necessary */
    lock_release (&entry->l);
  } else {
    /* Failsafe: bypass the cache */
    block_write (fs_device, sector, buf);
  }

  return BLOCK_SECTOR_SIZE;
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
 * Initializes an entry in the buffer cache table
 */
static bool
buffercache_allocate_block (struct cache_entry *entry)
{
  entry->kaddr = (uint32_t)palloc_get_page (0);
  if ((void *)entry->kaddr == NULL) return false;

  entry->state = READY;
  entry->accessed = CLEAN;

  lock_init (&entry->l);
  cond_init (&entry->c);

  return true;
}

/**
 * Flushes the specified cache entry to disk. Assumes the entry lock is
 * already held.
 */
static void
buffercache_flush_entry (struct cache_entry *entry)
{
}

/**
 * Invokes a read-ahead thread for the given cache entry if needed
 */
static void
buffercache_read_ahead_if_necessary (struct cache_entry *entry)
{
  int entry_num;

  /* No buddy */
  if (entry->next_sector == -1) return;

  /* Check if buddy is in cache already */

  entry_num = buffercache_find_entry (entry->next_sector);
  if (entry_num != -1) return;	/* Already pre-fetched */

  /* TODO Invoke pre-fetch */
}

static int
buffercache_find_entry (int sector)
{
  int i;
  for (i = 0; i < cache_size; i++)
  {
	
  }
  return -1;
}

/* Use the clock algorithm to find an entry to evict and flush it to
   disk */
static int buffercache_evict()
{
  	
	
  return -1;
}


/* Runs the clock algorithm to find the next entry to evict.  The cache
   lock should be held when calling this */
static int 
clock_algorithm(void)
{
  ASSERT(lock_held_by_current_thread(&cache_lock));
  int clock_start = clock_next ();
  struct cache_entry e = cache[clock_start];
	
  while (e.state == WRITING)
  {	
	e = cache[clock_next ()];
	if (clock_hand == clock_start) return NULL;
  }
  return clock_hand;
}

/* Helper function for the clock algorithm which treats the entries as a
   circularly linked list */
static int 
clock_next(void)
{
  clock_hand = clock_hand + 1;
  if (clock_hand == cache_size)
	clock_hand = 0;
  return clock_hand;
}
