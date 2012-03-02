#ifndef FILESYS_BUFFERCACHE_H
#define FILESYS_BUFFERCACHE_H

#include "threads/synch.h"

#define BUFFERCACHE_SIZE 64

/*
 * Flags to denote the state of a cache block
 */
enum cache_state
{
  WRITING,
  READY
};

/*
 * Names various states for buffer cache blocks
 */
enum cache_accessed
{
  CLEAN = 0x00,			/* Untouched */
  ACCESSED = 0x01,		/* Accessed bit */
  DIRTY = 0x02			/* Dirty bit */
};

/*
 * A single entry in the buffer cache
 */
struct cache_entry
{
  uint16_t kaddr;		/* Address of cache block */
  int next_sector;		/* Sector number of next sector */
  enum cache_state state;	/* Current state of block */
  enum cache_accessed accessed;	/* Accessed bits for block */
  struct lock l;		/* To lock the block */
  struct condition c;		/* To notify waiting threads */
};

bool buffercache_init (size_t size);
int buffercache_read (int sector, void *buf);
int buffercache_write (int sector, const void *buf);
bool buffercache_flush (void);

#endif
