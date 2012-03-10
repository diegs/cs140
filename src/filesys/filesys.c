#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/thread.h"
#include "threads/malloc.h"
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
filesys_create (const char *path, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  char *dirname = dir_dirname (path);
  const char *basename = dir_basename (path);
  struct dir *dir = dir_open_path (dirname);
  bool success = (dir != NULL
                  && basename != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, false)
                  && dir_add (dir, basename, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);
  if (dirname != NULL) free (dirname);
  return success;
}

/* Creates a directory at path. Returns true if successful, false
 * otherwise. Requires parent directories to exist */
bool
filesys_mkdir (const char *path)
{
  block_sector_t newdir_sector;
  char *dirname = dir_dirname (path);
  const char *basename = dir_basename (path);
  struct dir *dir = dir_open_path (dirname);
  bool success = (dir != NULL
                  && basename != NULL
                  && free_map_allocate (1, &newdir_sector)
                  && dir_create (newdir_sector, inode_get_inumber
                                 (dir_get_inode (dir)))
                  && dir_add_entry (dir, basename, newdir_sector));
  if (!success && newdir_sector != 0)
    free_map_release (newdir_sector, 1);
  dir_close (dir);
  if (dirname != NULL) free (dirname);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *path)
{
  char *dirname = dir_dirname (path);
  char *basename = dir_basename (path);
  struct dir *dir = dir_open_path (dirname);
  if (dirname != NULL) free (dirname);
  if (basename == NULL) return NULL;

  if (basename != NULL)
  {
    struct inode *inode = NULL;
    if (dir != NULL)
      dir_lookup (dir, basename, &inode);
    dir_close (dir);

    return file_open (inode);
  } else {
    if (dir != NULL) dir_close (dir);
    return NULL;
  }
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *path) 
{
  char *dirname = dir_dirname (path);
  char *basename = dir_basename (path);
  struct dir *dir = dir_open_path (dirname);
  if (dirname != NULL) free (dirname);

  bool success = false;
  if (basename != NULL) 
    success = dir != NULL && dir_remove (dir, basename);

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
