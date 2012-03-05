#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/buffercache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* The number of sector references in an inode */
#define INODE_NUM_BLOCKS 126
#define INODE_ROOT_DB 124
#define INODE_DIRECT_SIZE INODE_ROOT_DB*BLOCK_SECTOR_SIZE
#define INODE_INDIRECT_SIZE INODE_NUM_BLOCKS*BLOCK_SECTOR_SIZE
#define INODE_DUBINDER_SIZE INODE_INDIRECT_SIZE*INODE_NUM_BLOCKS
#define INODE_INDIRECT_OFFSET INODE_DIRECT_SIZE
#define INODE_DUBINDER_OFFSET INODE_INDIRECT_OFFSET + INODE_INDIRECT_SIZE

#define INODE_INVALID_BLOCK_SECTOR -1

static int level_sizes[] = { 1, INODE_DIRECT_SIZE, INODE_INDIRECT_SIZE, INODE_DUBINDER_SIZE };
static int level_offsets[] = { 0, INODE_INDIRECT_OFFSET, INODE_DUBINDER_OFFSET };
static int num_levels = sizeof (level_offsets) / sizeof (size_t);

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  block_sector_t sectors[INODE_NUM_BLOCKS];
  off_t length;                       /* File size in bytes. */
  unsigned magic;                     /* Magic number. */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode {
  block_sector_t disk_block;	/* Sector of this inode on disk*/
  struct list_elem elem;		/* Element in inode list. */
  
  off_t length;

  int open_cnt;					/* Number of openers. */
  bool removed;					/* True if deleted, false otherwise. */
  int deny_write_cnt;			/* 0: writes ok, >0: deny writes. */
};

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (struct inode *root, off_t pos, bool create) 
{
  ASSERT (root != NULL);

  block_sector_t cur_sector = root->disk_block;
  off_t cur_pos = pos;

  int i;
  for (i = num_levels - 1; i >= 0; i--) 
  {
    if (pos >= level_offsets[i]) 
    {
      off_t divisor = level_sizes[i];
      cur_pos = cur_pos - level_offsets[i];

      /* Get index into current block for next sector */
      block_sector_t next_sector;
      int index = cur_pos/divisor;

      ASSERT (index >= 0 && index < INODE_ROOT_DB);
      
      /* At the root level, we need to select the doubly indirect
         block and the singly indirect block manually */
      if (cur_sector == root->disk_block && i > 0)
        index = INODE_NUM_BLOCKS + i;

      off_t offset = offsetof(struct inode_disk, sectors) + index * sizeof (block_sector_t);

      /* Read next sector */
      int bytes_read = buffercache_read (cur_sector, METADATA, offset,
          sizeof (block_sector_t), &next_sector);
      if (bytes_read != sizeof (block_sector_t)) return -1;


	  /* Allocate a new sector if necessary*/
	  /* TODO: check that we can never allocate block 0 */
	  if (create && next_sector == 0)
	  {
		block_sector_t new_sector;
		bool allocated = free_map_allocate (1, &new_sector);
		if (!allocated) return -1;
		/* Update the current sector info */
		int bytes_written = buffercache_write (cur_sector, METADATA, offset, sizeof (block_sector_t), &next_sector);
		if (bytes_written != sizeof(block_sector_t)) return -1;
		next_sector = new_sector;
	  }
      cur_sector = next_sector;
    }
  }

  return cur_sector;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
  {
    size_t sectors = bytes_to_sectors (length);
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    block_sector_t sector;
    if (free_map_allocate (1, &sector)) 
    {
      buffercache_write (sector, METADATA, 0, BLOCK_SECTOR_SIZE, disk_inode);
      success = true; 
    } 
    free (disk_inode);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->disk_block == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->disk_block = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->disk_block;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->disk_block, 1);
		  /* TODO: close all blocks */
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, false);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Read chunk from this sector */
      buffercache_read (sector_idx, REGULAR, sector_ofs, chunk_size,
			buffer + bytes_read);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, true);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Write chunk to this sector. */
      buffercache_write (sector_idx, REGULAR, sector_ofs, chunk_size,
			 buffer + bytes_written);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->length;
}
