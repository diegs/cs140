#include <debug.h>
#include <string.h>

#include "devices/block.h"
#include "devices/timer.h"
#include "filesys/buffercache.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#define BUFFERCACHE_FLUSH_FREQUENCY 30 * 1000 /* 30 seconds */

static struct cache_entry *cache; /* Cache entry table */
static struct lock cache_lock;	  /* Lock for entry table */
static int cache_size;            /* Size of the cache */
static int clock_hand;            /* For clock algorithm */

static void buffercache_polling_thread (void *aux);
static void buffercache_allocate_block (struct cache_entry *entry, void *kaddr);
static struct cache_entry *buffercache_find_entry (const block_sector_t sector);
static struct cache_entry *buffercache_replace (const block_sector_t
                                                sector, enum sector_type type);
static int buffercache_read_direct (const block_sector_t sector,
                                    const int sector_ofs, const off_t size,
                                    void *buf);
static int buffercache_write_direct (const block_sector_t sector,
                                     const int sector_ofs, const off_t size,
                                     const void *buf);
static void buffercache_read_ahead_if_necessary (const block_sector_t sector);
static void buffercache_load_entry (struct cache_entry *entry,
                                    const block_sector_t sector,
                                    enum sector_type type);
static void buffercache_flush_entry (struct cache_entry *entry);
static struct cache_entry *buffercache_clock_algorithm (void);
static inline int buffercache_clock_next (void);

/**
 * Initializes the buffer cache system. Returns true on success, false on
 * error.
 */
bool
buffercache_init (const size_t size)
{
  int i;
  void *kaddr;
  tid_t t;

  /* Set the cache size */
  cache_size = size;

  /* Initialize list of pages */
  cache = malloc (cache_size * sizeof (struct cache_entry));
  if (cache == NULL) return false;
  lock_init (&cache_lock);

  /* Allocate the cache pages */
  for (i = 0; i < cache_size; i++)
  {
    if (i % (PGSIZE/BLOCK_SECTOR_SIZE) == 0)
    {
      /* Grab a new page */
      kaddr = palloc_get_page (0);
      if (kaddr == NULL) return false;
    } else {
      kaddr += BLOCK_SECTOR_SIZE;
    }

    buffercache_allocate_block (&cache[i], kaddr);
  }

  /* Initialize the clock hand so first access will be slot 0 */
  clock_hand = cache_size - 1;

  /* Create the buffercache flush thread */
  t = thread_create ("buffercache_polling_thread", PRI_DEFAULT,
                     buffercache_polling_thread, NULL);
  if (t == TID_ERROR) return false;

  return true;
}

/**
 * Reads a sector from sector into buf. Does not do bounds checking on
 * sector_ofs and size.
 *
 * Returns the number of bytes read, or -1 on failure.
 */
int
buffercache_read (const block_sector_t sector, enum sector_type type,
                  const int sector_ofs, const off_t size, void *buf)
{
  struct cache_entry *entry;

  /* Finds an entry and returns it with accessors incremented */
  lock_acquire (&cache_lock);
  entry = buffercache_find_entry (sector);
  if (entry == NULL)
    entry = buffercache_replace (sector, type);
  lock_release (&cache_lock);

  if (entry != NULL)
  {
    /* Read from cache entry */
    memcpy (buf, entry->kaddr + sector_ofs, size);

    /* Adjust cache entry */
    lock_acquire (&cache_lock);
    entry->accessed |= ACCESSED;
    if (entry->type == METADATA)
      entry->accessed |= META;
    entry->accessors--;
    if (entry->accessors == 0)
      cond_broadcast (&entry->c, &cache_lock);
    lock_release (&cache_lock);

    /* Trigger read-ahead */
    buffercache_read_ahead_if_necessary (sector);
    return size;
  } else {
    /* Failsafe: bypass the cache */
    return buffercache_read_direct (sector, sector_ofs, size, buf);
  }
}

/**
 * Writes a sector from buf into sector. Does not do bounds checking on
 * sector_ofs and size.
 *
 * Returns the number of bytes written, or -1 on failure.
 */
int
buffercache_write (const block_sector_t sector, enum sector_type type,
                   const int sector_ofs, const off_t size, const void *buf)
{
  struct cache_entry *entry;

  /* Finds an entry and returns it with accessors incremented */
  lock_acquire (&cache_lock);
  entry = buffercache_find_entry (sector);
  if (entry == NULL)
    entry = buffercache_replace (sector, type);
  lock_release (&cache_lock);

  if (entry != NULL)
  {
    /* Write to cache entry */
    memcpy (entry->kaddr + sector_ofs, buf, size);

    /* Adjust cache entry */
    lock_acquire (&cache_lock);
    entry->accessed |= ACCESSED | DIRTY;
    if (entry->type == METADATA)
      entry->accessed |= META;
    entry->accessors--;
    if (entry->accessors == 0)
      cond_broadcast (&entry->c, &cache_lock);
    lock_release (&cache_lock);

    /* Trigger read-ahead and return */
    buffercache_read_ahead_if_necessary (sector);
    return size;
  } else {
    /* Failsafe: bypass the cache */
    return buffercache_write_direct (sector, sector_ofs, size, buf);
  }
}

/**
 * Flushes all dirty buffers in the cache to disk.
 */
void
buffercache_flush (void)
{
  int i;

  for (i = 0; i < cache_size; i++)
  {
    lock_acquire (&cache_lock);
    buffercache_flush_entry (&cache[i]);
    lock_release (&cache_lock);
  }
}

/**
 * Daemon thread that flushes all buffers to disk every 30 seconds.
 */
static void
buffercache_polling_thread (void *aux UNUSED)
{
  while (true)
  {
    timer_msleep (BUFFERCACHE_FLUSH_FREQUENCY);
    buffercache_flush ();
  }
}

/**
 * Initializes an entry in the buffer cache table.
 */
static void
buffercache_allocate_block (struct cache_entry *entry, void *kaddr)
{
  entry->kaddr = kaddr;
  entry->accessors = 0;
  entry->sector = -1;
  entry->state = READY;
  entry->accessed = CLEAN;
  entry->type = REGULAR;
  cond_init (&entry->c);
}

/**
 * Writes directly to disk, bypassing the buffer cache. Used under failsafe
 * conditions.
 */
static int
buffercache_write_direct (const block_sector_t sector, const int sector_ofs,
                          const off_t size, const void *buf)
{
  static void *bounce;

  /* Allocate bounce buffer */
  bounce = malloc (BLOCK_SECTOR_SIZE);
  if (bounce == NULL) return -1;

  /* Read from disk */
  block_read (fs_device, sector, bounce);

  /* Copy data into bounce buffer */
  memcpy (bounce + sector_ofs, buf, size);

  /* Write back to disk */
  block_write (fs_device, sector, bounce);

  free (bounce);
  return size;
}

/**
 * Reads directly from disk, bypassing the buffer cache. Used under failsafe
 * conditions.
 */
static int
buffercache_read_direct (const block_sector_t sector, const int sector_ofs,
                         const off_t size, void *buf)
{
  static void *bounce;

  /* Allocate bounce buffer */
  bounce = malloc (BLOCK_SECTOR_SIZE);
  if (bounce == NULL) return -1;

  /* Read from disk */
  block_read (fs_device, sector, bounce);

  /* Copy into buffer */
  memcpy (buf, bounce + sector_ofs, size);

  free (bounce);
  return size;
}

/**
 * Flushes the specified cache entry to disk (if necessary).
 *
 * Requires the cache_lock to be held.
 */
static void
buffercache_flush_entry (struct cache_entry *entry)
{
  ASSERT (lock_held_by_current_thread (&cache_lock));

  /* Only flush a dirty entry that is fully read/written */
  if (entry->accessed & DIRTY && (entry->state == READY || entry->state == CLOCK))
  {
    /* Wait for current accessors to finish */
    entry->state = WRITE_REQUESTED;
    while (entry->accessors > 0)
      cond_wait (&entry->c, &cache_lock);

    /* Write to disk */
    entry->state = WRITING;	/* Tell threads block is writing */
    lock_release (&cache_lock);

    /* Perform I/O */
    block_write (fs_device, entry->sector, entry->kaddr);

    /* Fix up entry */
    lock_acquire (&cache_lock);
    entry->state = READY;	/* No longer writing */
    entry->accessed &= ~DIRTY;	/* No longer dirty */
    cond_broadcast (&entry->c, &cache_lock); /* Tell threads writing is done */
  }
}

/**
 * Invokes a read-ahead thread for the given block.
 */
static void
buffercache_read_ahead_if_necessary (const block_sector_t sector UNUSED)
{
  
  /* TODO implement pre-fetch */
  return;
}

/**
 * Returns the cache entry for the given sector if it is cached,
 * else NULL.
 */
static struct cache_entry *
buffercache_find_entry (const block_sector_t sector)
{
  int i;

  ASSERT (lock_held_by_current_thread (&cache_lock));

  for (i = 0; i < cache_size; i++)
  {
    if (cache[i].sector == sector)
    {
      /* If it's being read or written, wait */
      while (cache[i].state != READY)
        cond_wait (&cache[i].c, &cache_lock);

      /* Double-check in case it was replaced */
      if (cache[i].sector != sector)
      {
        i = 0;                  /* Restart search */
        continue;
      } else {
        cache[i].accessors++;   /* Prevent replacement */
        return &cache[i];
      }
    }
  }

  return NULL;
}

/**
 * Use the clock algorithm to find an entry to replace (if necessary) and
 * flush it to disk (also if necessary) and load in a new sector.
 */
static struct cache_entry *
buffercache_replace (const block_sector_t sector, enum sector_type type)
{
  struct cache_entry *e;

  ASSERT (lock_held_by_current_thread (&cache_lock));

  e = buffercache_clock_algorithm ();	/* Marks as WRITE_REQUESTED */
  if (e == NULL) return NULL;

  buffercache_flush_entry (e);              /* Write current entry */
  buffercache_load_entry (e, sector, type); /* Read new entry into buffer */
  e->accessors++;                           /* Prevent replacement */

  return e;
}

/**
 * Loads a disk sector into a buffer. Expects the cache lock to be acquired
 * when called, releases it during I/O, and re-acquires it before returning.
 */
static void
buffercache_load_entry (struct cache_entry *entry, const block_sector_t
                        sector, enum sector_type type)
{
  ASSERT (lock_held_by_current_thread (&cache_lock));
  ASSERT (entry->accessors == 0);

  /* Fix cache entry */
  entry->state = READING;
  entry->sector = sector;
  entry->accessed = CLEAN;
  entry->type = type;
  lock_release (&cache_lock);

  /* Perform I/O */
  block_read (fs_device, entry->sector, entry->kaddr);

  /* Re-acquire cache lock */
  lock_acquire (&cache_lock);

  /* Ready to be used */
  entry->state = READY;
}

/**
 * Runs the clock algorithm to find the next entry to replace. The cache
 * lock must be held when calling this.
 *
 * The algorithm proceeds as follows: for each advancement of the clock,
 * if the entry is locked it is ignored. If the accessed bit is set it is
 * reset. If the accessed bit is not set then this entry is returned. If
 * we wrap around then the clock hand is returned.
 *
 * An additional "chance" (i.e. this becomes a "3rd-chance" algorithm) is given
 * for metadata blocks, since those are more valuable to keep in the cache.
 *
 * Returns a locked entry that is ready to be flushed to disk and replaced.
 */
static struct cache_entry *
buffercache_clock_algorithm (void)
{
  int clock_start;
  struct cache_entry *e;

  ASSERT (lock_held_by_current_thread (&cache_lock));

  clock_start = buffercache_clock_next ();
  while (cache[clock_start].state != READY)
    clock_start = buffercache_clock_next ();

  e = &cache[clock_start];

  do
  {
    /* Ignore if some I/O is happening to this block, ignore */
    if (e->state == READY)
    {
      if (e->accessed & ACCESSED) {
        /* Access bit is set, unset it and continue */
        e->accessed &= ~ACCESSED;
      } else if (e->accessed & META) {
        /* Give metadata blocks another chance */
        e->accessed &= ~META;
      } else {
        /* Access and meta bits not set, return this entry */
        break;
      }
    }
    e = &cache[buffercache_clock_next ()];
  } while (clock_hand != clock_start);

  /* Claim this entry for ourselves */
  e->state = CLOCK;
  return e;
}

/**
 * Helper function for the clock algorithm which treats the entries as a
 * circularly linked list.
 */
static inline int
buffercache_clock_next (void)
{
  return (clock_hand = (clock_hand + 1) % cache_size);
}
