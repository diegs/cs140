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
#define INODE_CONSISTENT_BLOCKS 122
#define INODE_DIRECT_SIZE (INODE_CONSISTENT_BLOCKS*BLOCK_SECTOR_SIZE)
#define INODE_INDIRECT_SIZE (INODE_CONSISTENT_BLOCKS*BLOCK_SECTOR_SIZE)
#define INODE_DUBINDER_SIZE (INODE_INDIRECT_SIZE*INODE_CONSISTENT_BLOCKS)

#define INODE_NUM_INDIRECT_BLOCKS 1
#define INODE_NUM_DUBINDER_BLOCKS 2

#define INODE_INDIRECT_OFFSET INODE_DIRECT_SIZE
#define INODE_DUBINDER_OFFSET (INODE_INDIRECT_OFFSET+INODE_NUM_INDIRECT_BLOCKS*INODE_INDIRECT_SIZE)

#define INODE_INDIRECT_INDEX_BASE INODE_CONSISTENT_BLOCKS
#define INODE_DUBINDER_INDEX_BASE (INODE_CONSISTENT_BLOCKS+INODE_NUM_INDIRECT_BLOCKS)

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

/* Returns the sector at index if it is a valid sector or if create is 
   true */
static block_sector_t
verify_sector (block_sector_t cur_sector, int index, bool
    is_direct_level, bool create)
{
  if (cur_sector == INODE_INVALID_BLOCK_SECTOR) 
    return INODE_INVALID_BLOCK_SECTOR;

  block_sector_t next_sector = 
    get_sector_from_block (cur_sector, index);

  /* Allocate a new sector if necessary*/
  if (next_sector == INODE_INVALID_BLOCK_SECTOR && create) {
    enum sector_type type = is_direct_level ? REGULAR : METADATA;
    next_sector = create_new_sector (cur_sector, index, type);
  }
  
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

  block_sector_t indirect_sector = root->disk_block;
  block_sector_t dubindirect_sector = INODE_INVALID_BLOCK_SECTOR;
  block_sector_t result = INODE_INVALID_BLOCK_SECTOR;
  off_t cur_pos = pos;

  /* If we did not get a sector index, but we are still within
     the length of the file, we can create a new block */
  bool within_length = pos < root->length;
  bool create_final = create || within_length;

  /* Move down from the doubly indirect level if needed */
  if (cur_pos >= INODE_DUBINDER_OFFSET)
  {
    cur_pos -= INODE_DUBINDER_OFFSET;
    int dubinder_index = 
        INODE_DUBINDER_INDEX_BASE + cur_pos/INODE_DUBINDER_SIZE;

    cur_pos %= INODE_DUBINDER_SIZE;
    dubindirect_sector = verify_sector (root->disk_block,
        dubinder_index, false, create_final);
  }

  /* Move down from the singly indirect level if needed the first 
   case handles the case in which we still need to step off of the
   root block into the indirect block and the other deals with 
   stepping forward in the case we are in the doubly indirect path.*/
  int indir_index = -1;
  if (cur_pos >= INODE_INDIRECT_OFFSET 
      && dubindirect_sector == INODE_INVALID_BLOCK_SECTOR) 
  {
    cur_pos -= INODE_INDIRECT_OFFSET;
    indir_index = 
      INODE_INDIRECT_INDEX_BASE + (cur_pos/INODE_INDIRECT_SIZE);
    dubindirect_sector = root->disk_block;
  } else if (dubindirect_sector != INODE_INVALID_BLOCK_SECTOR) {
    indir_index = cur_pos /INODE_INDIRECT_SIZE;
  }

  /* Actually make the step at the indirect level */
  if (indir_index != -1)
  {
    cur_pos %= INODE_INDIRECT_SIZE;
    indirect_sector = verify_sector (dubindirect_sector, indir_index,
        false, create_final); 
  }

  /* Find final block */
  if (cur_pos < INODE_DIRECT_SIZE) 
  {
    int direct_index = cur_pos/BLOCK_SECTOR_SIZE;
    result = verify_sector (indirect_sector, direct_index,
        true, create_final); 
  }

  lock_release (&root->lock);

  return result;
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

  if (indirect_sector != INODE_INVALID_BLOCK_SECTOR)
  {
    while (bytes_traversed < length 
            && block_index < INODE_CONSISTENT_BLOCKS)
    {
      sector = get_sector_from_block (indirect_sector, block_index);
      if (sector != INODE_INVALID_BLOCK_SECTOR) map_fn (sector, false);

      block_index++;
      bytes_traversed += BLOCK_SECTOR_SIZE;
    }

  }

  return bytes_traversed;
}

static void
inode_sector_map (struct inode *root, inode_sector_map_fn map_fn)
{
  ASSERT (root != NULL);
  ASSERT (root->disk_block != INODE_INVALID_BLOCK_SECTOR);

  off_t bytes_traversed = 0;

  /* Iterate over all direct blocks in the root level */
  bytes_traversed = inode_sector_map_direct_helper (root->disk_block,
    bytes_traversed, root->length, map_fn);
  
  int i;

  /* Iterate over all indirect blocks at the root level */
  for (i = 0; i < INODE_NUM_INDIRECT_BLOCKS; i++)
  {
    int indir_index = INODE_INDIRECT_INDEX_BASE + i;
    block_sector_t indirect_sector = get_sector_from_block 
      (root->disk_block, indir_index);

    bytes_traversed = inode_sector_map_direct_helper (indirect_sector,
        bytes_traversed, root->length, map_fn);

    if (indirect_sector != INODE_INVALID_BLOCK_SECTOR) 
      map_fn (indirect_sector, false);
  }

  /* Iterate over all the doubly indirect blocks at root level */
  for (i = 0; i < INODE_NUM_DUBINDER_BLOCKS; i++)
  {
    int dubinder_index = INODE_DUBINDER_INDEX_BASE + i;
    block_sector_t dubinder_sector = get_sector_from_block
      (root->disk_block, dubinder_index);

    int indir_index = 0;
    while (bytes_traversed < root->length 
        && indir_index < INODE_CONSISTENT_BLOCKS)
    {
      block_sector_t indirect_sector = 
        get_sector_from_block (dubinder_sector, indir_index);
      bytes_traversed = inode_sector_map_direct_helper
        (indirect_sector, bytes_traversed, root->length, map_fn);
      indir_index++;
      if (indirect_sector != INODE_INVALID_BLOCK_SECTOR) 
        map_fn (indirect_sector, false);
    }

    if (dubinder_sector != INODE_INVALID_BLOCK_SECTOR)
      map_fn (dubinder_sector, true);
  }

  /* Map over the root inode sector */
  map_fn (root->disk_block, true);
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
                               sizeof (off_t) + sizeof (bool),
                               &inode->length,
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

  /* Handle file extension */
  lock_acquire (&inode->lock);
  if (offset + bytes_written > inode->length) inode->length = offset;
  lock_release (&inode->lock);

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
