#ifndef FILESYS_BUFFERCACHE_H
#define FILESYS_BUFFERCACHE_H

#include "devices/block.h"
#include "filesys/off_t.h"
#include "threads/synch.h"

#define BUFFERCACHE_SIZE 64

/*
 * Flags to denote the state of a cache block
 */
enum cache_state
{
  READING,
  WRITE_REQUESTED,
  WRITING,
  CLOCK,
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
  void *kaddr;			/* Address of cache block */
  int accessors;		/* Number of threads accessing buffer */
  block_sector_t sector;	/* Sector of block */
  enum cache_state state;	/* Current state of block */
  enum cache_accessed accessed;	/* Accessed bits for block */
  struct condition c;		/* To notify waiting threads */
};

bool buffercache_init (const size_t size);
int buffercache_read (const block_sector_t sector, const int sector_ofs,
		      const off_t size, void *buf);
int buffercache_write (const block_sector_t sector, const int sector_ofs,
		       const off_t size, const void *buf);
void buffercache_flush (void);

#endif
