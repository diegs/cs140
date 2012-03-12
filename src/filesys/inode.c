#include "filesys/inode.h"
#include <stdio.h>
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
#define INODE_NUM_BLOCKS 125
#define INODE_CONSISTENT_BLOCKS 123
#define INODE_DIRECT_SIZE INODE_CONSISTENT_BLOCKS*BLOCK_SECTOR_SIZE
#define INODE_INDIRECT_SIZE INODE_CONSISTENT_BLOCKS*BLOCK_SECTOR_SIZE
#define INODE_DUBINDER_SIZE INODE_INDIRECT_SIZE*INODE_CONSISTENT_BLOCKS
#define INODE_INDIRECT_OFFSET INODE_DIRECT_SIZE
#define INODE_DUBINDER_OFFSET INODE_INDIRECT_OFFSET + INODE_INDIRECT_SIZE

#define INODE_ROOT_INDIRECT_INDEX INODE_CONSISTENT_BLOCKS
#define INODE_ROOT_DUBINDER_INDEX INODE_CONSISTENT_BLOCKS + 1

static int level_sizes[] = { BLOCK_SECTOR_SIZE, INODE_DIRECT_SIZE, INODE_DUBINDER_SIZE };
static int level_offsets[] = { 0, INODE_INDIRECT_OFFSET, INODE_DUBINDER_OFFSET };
static int num_levels = sizeof (level_offsets) / sizeof (size_t);

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  /* All of the inode blocks contains INODE_CONSISTENT_BLOCKS of 
     blocks for the next level of indirection. The root block uses
     INODE_CONSISTENT_BLOCKS + 1 for the singly indirect block and
     INODE_CONSISTENT_BLOCKS + 2 for the doubly indirect block */
  block_sector_t sectors[INODE_NUM_BLOCKS];
  off_t length;                 /* File size in bytes. */
  bool directory;               /* true if this inode represents a directory */
  uint8_t padding[3];           /* padding */
  unsigned magic;               /* Magic number. */
};

void inode_sector_free_map_fn (block_sector_t sector, bool meta);

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode {
  block_sector_t disk_block;    /* Sector of this inode on disk*/
  struct list_elem elem;        /* Element in inode list. */
  off_t length;
  bool directory;               /* true if this inode represents a directory */
  int open_cnt;                 /* Number of openers. */
  bool removed;                 /* True if deleted, false otherwise. */
  int deny_write_cnt;           /* 0: writes ok, >0: deny writes. */
  int deny_remove_cnt;          /* 0: removes ok, >0: deny removes.*/
  struct lock lock;
};

static off_t index_to_offset (int index)
{
  return offsetof(struct inode_disk, sectors) 
    + index*sizeof (block_sector_t);
}

static block_sector_t 
create_new_sector (block_sector_t cur_sector, int index, 
    enum sector_type type)
{
  off_t offset = index_to_offset (index);
  block_sector_t new_sector;
  bool allocated = free_map_allocate (1, &new_sector);
  if (!allocated) return -1;

  /* Update the current sector info */
  int bytes_written = buffercache_write (cur_sector, METADATA, offset,
                                         sizeof (block_sector_t), &new_sector,
                                         INODE_INVALID_BLOCK_SECTOR);
  if (bytes_written != sizeof(block_sector_t)) return -1;

  /* Correctly initialize the new sector -- it should either
     be all zeros if it is newly created or filled with 
     INODE_INVALID_BLOCK_SECTOR otherwise */
  int fill = (type == METADATA) ? INODE_INVALID_BLOCK_SECTOR : 0;
  unsigned j;
  
  int *kernel_block = (int*)malloc (BLOCK_SECTOR_SIZE);
  for (j = 0; j < BLOCK_SECTOR_SIZE/sizeof(int); j++)
    kernel_block[j] = fill;

  buffercache_write (new_sector, type, 0, BLOCK_SECTOR_SIZE,
                     kernel_block, INODE_INVALID_BLOCK_SECTOR);

  free (kernel_block);

  return new_sector;
}

static block_sector_t
get_sector_from_block (block_sector_t sector, int index)
{
  if (sector == INODE_INVALID_BLOCK_SECTOR)
    return INODE_INVALID_BLOCK_SECTOR;

  off_t offset = index_to_offset (index);
  block_sector_t next_sector;
  int bytes_read = buffercache_read (sector, METADATA, offset,
      sizeof (block_sector_t), &next_sector,
      INODE_INVALID_BLOCK_SECTOR); 

  if (bytes_read != sizeof (block_sector_t)) 
    return INODE_INVALID_BLOCK_SECTOR;

  return next_sector;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (struct inode *root, off_t pos, bool create) 
{
  ASSERT (root != NULL);

  lock_acquire (&root->lock);
  block_sector_t cur_sector = root->disk_block;
  off_t cur_pos = pos;

  int i;
  bool root_block = true;
  for (i = num_levels - 1; i >= 0; i--) 
  {
    /* Figure out the index from which we would like to read 
       the next sector index */
    int index = -1;
    block_sector_t next_sector;
    if (root_block && cur_pos >= level_offsets[i] && i > 0) 
    {
      /* At the root level, we need to select the doubly indirect
         block and the singly indirect block manually */
      index = INODE_CONSISTENT_BLOCKS + i - 1;
      cur_pos = cur_pos - level_offsets[i];
      root_block = false;
    } else if ((i < 2 && !root_block && cur_pos < level_sizes[i+1]) 
                || i == 0) {
      off_t divisor = level_sizes[i];

      /* Get index into current block for next sector */
      index = cur_pos/divisor;
      cur_pos = cur_pos - level_sizes[i]*index;

      ASSERT (index >= 0 && index < INODE_CONSISTENT_BLOCKS);
    }

    /* Perform the traversal if we need to perform one at this level
       of indirection */
    if (index != -1) 
    {
      next_sector = get_sector_from_block (cur_sector, index);

      /* Allocate a new sector if necessary*/
      if (next_sector == INODE_INVALID_BLOCK_SECTOR) {
      /* If we did not get a sector index, but we are still within
         the length of the file, we can zero out a block size of 
         data in the buffer */
        bool within_length = pos < root->length;

        if (create || within_length)
        {
          enum sector_type type = i > 0 ? METADATA : REGULAR;
          next_sector = create_new_sector (cur_sector, index, type);
        } else {
          cur_sector = INODE_INVALID_BLOCK_SECTOR;
          break;
        }
      }
      cur_sector = next_sector;
    }
  }

  lock_release (&root->lock);

  return cur_sector;
}

typedef void (*inode_sector_map_fn) (block_sector_t sector, bool meta);


/* Iterates through all direct blocks in the given sector and applies
  the mapping function to them. */
static off_t
inode_sector_map_direct_helper (block_sector_t indirect_sector, 
  off_t bytes_traversed, off_t length, inode_sector_map_fn map_fn)
{
  int block_index = 0;
  block_sector_t sector;
  while (bytes_traversed < length 
          && block_index < INODE_CONSISTENT_BLOCKS)
  {
    sector = get_sector_from_block (indirect_sector, block_index);
    map_fn (sector, false);

    block_index++;
    bytes_traversed += BLOCK_SECTOR_SIZE;
  }
  return bytes_traversed;
}
  

static void
inode_sector_map (struct inode *root, inode_sector_map_fn map_fn)
{
  ASSERT (root != NULL);

  off_t bytes_traversed = 0;

  /* Iterate over all direct blocks in the root level */
  bytes_traversed = inode_sector_map_direct_helper (root->disk_block,
    bytes_traversed, root->length, map_fn);
  
  /* Since the first indirect block is it's own block, we set the 
    index high so that it rolls over to the doubly indirect block */
  int dubinder_index = INODE_CONSISTENT_BLOCKS;

  block_sector_t inder_block = get_sector_from_block 
    (root->disk_block, INODE_ROOT_INDIRECT_INDEX);
  block_sector_t dubinder_block = get_sector_from_block 
    (root->disk_block, INODE_ROOT_DUBINDER_INDEX);

  /* This loop handles all singly indirect blocks and their children.
     After this loop completes, only the root block and the doubly
     indirect block have not been passed in. */ 
  bool end_of_file = false;
  while (!end_of_file)
  {
    bytes_traversed = inode_sector_map_direct_helper
      (inder_block, bytes_traversed, root->length, map_fn);

    end_of_file = (bytes_traversed >= root->length);

    /* Handle a partially filled blocks*/
    if (end_of_file) 
    {
      if (inder_block != INODE_INVALID_BLOCK_SECTOR)
        map_fn (inder_block, true);

      if (dubinder_block != INODE_INVALID_BLOCK_SECTOR) 
        map_fn (dubinder_block, true);

      /* Pass in the sector that contains the root inode */
      map_fn (root->disk_block, true);
      break;
    }

    /* Update indirect index in doubly indirect block */
    dubinder_index++;
    if (dubinder_index >= INODE_CONSISTENT_BLOCKS)
      dubinder_index = 0;

    if (inder_block != INODE_INVALID_BLOCK_SECTOR)
      map_fn (inder_block, true);

    inder_block = get_sector_from_block 
      (dubinder_block, dubinder_index); 
  }

}

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
inode_create (block_sector_t sector, off_t length, const bool directory)
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
    memset(disk_inode, INODE_INVALID_BLOCK_SECTOR, INODE_NUM_BLOCKS * sizeof(block_sector_t));
    disk_inode->length = length;
    disk_inode->directory = directory;
    disk_inode->magic = INODE_MAGIC;
    int wrote = buffercache_write (sector, METADATA, 0, BLOCK_SECTOR_SIZE,
                                   disk_inode, INODE_INVALID_BLOCK_SECTOR);
    success = (wrote == BLOCK_SECTOR_SIZE);
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
  printf("opening %u\n", sector);
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
  /* Read length and directory flag from block */
  int read = buffercache_read (sector, METADATA,
                               offsetof (struct inode_disk, length),
                               sizeof (off_t) + sizeof (bool), &inode->length,
                               INODE_INVALID_BLOCK_SECTOR);
  if (read != (sizeof (off_t) + sizeof (bool)))
  {
    free(inode);
    return NULL;
  }
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->lock);
  return inode;
}


void inode_sector_free_map_fn (block_sector_t sector, bool meta UNUSED)
{
  free_map_release (sector, 1);
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  lock_acquire (&inode->lock);
  if (inode != NULL)
    inode->open_cnt++;
  lock_release (&inode->lock);
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
  printf("closing inode %u\n", inode->disk_block);
  if (inode == NULL)
    return;

  lock_acquire (&inode->lock);
  inode->open_cnt--;
  lock_release (&inode->lock);
  /* Release resources if this was the last opener. */
  if (inode->open_cnt == 0)
  {
    /* Remove from inode list and release lock. */
    list_remove (&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) 
    {
      inode_sector_map (inode, inode_sector_free_map_fn);
    }
    buffercache_write (inode->disk_block, METADATA,
                       offsetof (struct inode_disk, length), sizeof (off_t) +
                       sizeof (bool), &inode->length,
                       INODE_INVALID_BLOCK_SECTOR);
    free (inode); 
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
bool
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  if (inode->deny_remove_cnt > 0 && !inode->removed) return false;
  inode->removed = true;
  return true;
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
      if (sector_idx == INODE_INVALID_BLOCK_SECTOR) break;

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
      int read = buffercache_read (sector_idx, REGULAR, sector_ofs,
                                   chunk_size, buffer + bytes_read,
                                   byte_to_sector (inode, offset+chunk_size,
                                                   false));
      /* Advance. */
      size -= read;
      offset += read;
      bytes_read += read;
      if (read != chunk_size) break;
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
	  if (sector_idx == INODE_INVALID_BLOCK_SECTOR) break;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < sector_left ? size : sector_left;
      if (chunk_size <= 0)
        break;

      /* Write chunk to this sector. */
      int wrote = buffercache_write (sector_idx, REGULAR, sector_ofs,
                                     chunk_size, buffer + bytes_written,
                                     byte_to_sector (inode, offset+chunk_size,
                                                     false));
      /* Advance. */
      size -= wrote;
      offset += wrote;
      bytes_written += wrote;
	  if (wrote != chunk_size) break;
    }
  if (offset + bytes_written > inode->length) inode->length = offset;
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire (&inode->lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release (&inode->lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_acquire (&inode->lock);
  inode->deny_write_cnt--;
  lock_release (&inode->lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->length;
}

/* Returns true if this inode represents a directory */
bool
inode_is_directory (const struct inode *inode)
{
  return inode->directory;
}

bool inode_is_removed (const struct inode *i)
{
  return i->removed;
}

void inode_deny_remove (struct inode *inode)
{
  lock_acquire (&inode->lock);
  inode->deny_remove_cnt++;
  lock_release (&inode->lock);
}

void inode_allow_remove (struct inode *inode)
{
  lock_acquire (&inode->lock);
  inode->deny_remove_cnt--;
  lock_release (&inode->lock);
}
