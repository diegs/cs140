#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/thread.h"
#include "filesys/buffercache.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();
  if (!buffercache_init (BUFFERCACHE_SIZE))
    PANIC ("Could not create buffer cache, can't initialize file system.");

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  buffercache_flush (true);
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir = dir_open_root (); /* TODO fix. */
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, false)
                  && dir_add (dir, name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Creates a directory at path. Returns true if successful, false
 * otherwise. Requires parent directories to exist */
bool
filesys_mkdir (const char *path)
{
  block_sector_t sector;
  block_sector_t newdir_sector;
  char *basename;
  struct inode *i;
  bool status;

  /* Get sector for path up to new entry */
  sector = path_get_dirname_sector (path);
  if (sector == INODE_INVALID_BLOCK_SECTOR) return false;

  /* Get basename of entry to add */
  basename = path_get_basename (path);
  if (basename == NULL) return false;

  /* Make new directory entry */
  status = (free_map_allocate (1, &newdir_sector)
             && dir_create (newdir_sector, sector));
  if (!status && newdir_sector != 0)
  {
    free_map_release (newdir_sector, 1);
    return false;
  }  

  /* Insert directory entry into parent */
  i = inode_open (sector);
  if (i == NULL)
  {
    free_map_release (newdir_sector, 1);
    return false;
  }

  status = dir_add_entry (i, basename, newdir_sector);
  inode_close (i);
  return status;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir = dir_open_root ();
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, name, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir = dir_open_root ();
  bool success = dir != NULL && dir_remove (dir, name);
  dir_close (dir); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (free_map_root_sector (), free_map_root_sector ()))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
