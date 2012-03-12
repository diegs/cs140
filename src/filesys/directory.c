#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/buffercache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* A directory. */
struct dir 
{
  struct inode *inode;   /* Backing store. */
  off_t pos;             /* Current position. */
};

/* A single directory entry. */
struct dir_entry 
{
  block_sector_t inode_sector;        /* Sector number of header. */
  char name[NAME_MAX + 1];            /* Null terminated file name. */
  bool in_use;                        /* In use or free? */
};

static bool lookup (const struct dir *dir, const char *name,
                    struct dir_entry *ep, off_t *ofsp);
static size_t dir_size (struct dir *dir);

/* Walk through directory looking for a free entry. If no free entries,
 * append one to the end. */
bool
dir_add_entry (struct dir *dir, const char *name, block_sector_t sector)
{
  struct dir_entry entry;
  struct dir_entry e;
  int pos;
  off_t bytes;

  /* Make sure doesn't already exist */
  if (lookup (dir, name, NULL, NULL)) return false;

  strlcpy (entry.name, name, NAME_MAX);
  entry.inode_sector = sector;
  entry.in_use = true;
  pos = 0;

  /* Find a free entry */
  while (inode_read_at (dir->inode, &e, sizeof e, pos) == sizeof e) 
  {
    if (!e.in_use) break;
    pos += sizeof e;
  }

  bytes = inode_write_at (dir->inode, &entry, sizeof (struct dir_entry), pos);
  return (bytes == sizeof (struct dir_entry));
}

/* Creates a directory in the given SECTOR.  Returns true if successful, false
   on failure. */
bool
dir_create (block_sector_t sector, block_sector_t parent)
{
  bool status;
  struct dir *dir;

  /* Create sector with enough room for two entries */
  status = inode_create (sector, 2 * sizeof (struct dir_entry), true);
  if (!status) return status;

  /* Add entries for '.' and '..' */
  dir = dir_open (inode_open (sector));
  if (dir == NULL) return false;

  dir_add_entry (dir, ".", sector);
  dir_add_entry (dir, "..", parent);

  dir_close (dir);

  return true;
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
  {
    dir->inode = inode;
    dir->pos = 2 * sizeof (struct dir_entry);
    return dir;
  }
  else
  {
    inode_close (inode);
    free (dir);
    return NULL; 
  }
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL && dir_size(dir) == 2)
  {
	/* Don't close if it contains items other than . and .. */
    inode_close (dir->inode);
    free (dir);
  }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
    {
      if (ep != NULL)
        *ep = e;
      if (ofsp != NULL)
        *ofsp = ofs;
      return true;
    }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);

  if (name == NULL)
    *inode = inode_open (inode_get_inumber (dir->inode));
  else if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  success = inode_remove (inode);

done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  bool result = false;
  while (!result && 
      inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
  {
    if (e.in_use)
    {
      strlcpy (name, e.name, NAME_MAX + 1);
      result = true;
    } 
    dir->pos += sizeof e;
  }
  return result;
}

/* Gets the directory component of the given path. Returns a new string that
 * must be free'd later. */
char *
dir_dirname (const char *path)
{
  char *dirpath, *end;
  int len;

  /* Remove trailing stuff */
  end = strrchr (path, '/');

  /* No slashes, just return NULL */ 
  if (end == NULL) return NULL;

  /* Make a copy and return */
  len = end - path + 1;
  dirpath = malloc (len + 1);
  if (dirpath == NULL) return NULL;
  strlcpy (dirpath, path, len + 1);
  return dirpath;
}

/* Opens the directory for the given path. If the path is null, opens the
 * CWD */
struct dir *
dir_open_path (const char *path)
{
  block_sector_t sector = path_traverse (path, NULL);
  if (sector == INODE_INVALID_BLOCK_SECTOR) return NULL;
  struct dir *d = dir_open (inode_open (sector));
  if (inode_is_removed (d->inode)) return NULL;
  return d;
}

/* Traverse -- if thread is non-NULL, update the path of the thread */
block_sector_t
path_traverse (char *path, struct thread *t)
{
  char *token, *save_ptr;
  bool found;
  struct dir dir;
  struct dir_entry entry;
  block_sector_t sector;

  if (path == NULL) return thread_get_cwd ();

  /* Check if absolute path */
  if (path[0] == '/')
  {
    sector = free_map_root_sector ();
    path++;

    if (t != NULL) thread_clear_dirs (t);
  } else {
    sector = thread_get_cwd ();
  }

  for (token = strtok_r (path, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
  {
    /* Open the inode for this directory */
    dir.inode = inode_open (sector);
    dir.pos = 0;
    if (dir.inode == NULL || !inode_is_directory (dir.inode))
    {
      sector = INODE_INVALID_BLOCK_SECTOR;
      inode_close (dir.inode);
      break;
    }
    
    /* Make the thread leave the directory if applicable */
    if (t != NULL) 
      thread_leave_dir (t, dir.inode);

    /* Look up in current directory */
    found = lookup (&dir, token, &entry, NULL);
    if (found)
    {
      sector = entry.inode_sector;

      /* Make thread enter the directory if applicable */
      if (t != NULL)
        thread_enter_dir (t, dir.inode);

      inode_close (dir.inode);
    } else {
      sector = INODE_INVALID_BLOCK_SECTOR;
      inode_close (dir.inode);
      break;
    }
  }

  return sector;
}

/* Returns the basename for the given path. Does not make a copy. */
const char *
dir_basename (const char *path)
{
  char *start;
  const char *basename;

  start = strrchr (path, '/');
  if (start == NULL)
    basename = path;
  else
    basename = start + 1;

  if (basename == NULL || strlen (basename) == 0 ||
      strlen (basename) > NAME_MAX)
    return NULL;

  return basename;
}

/* Returns the number of entries in this directory, including '.' and
   '..' */
static size_t
dir_size (struct dir *dir)
{
  struct dir_entry e;
  size_t ofs;
  size_t count;

  ASSERT (dir != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
  {
    if (e.in_use)
      count++;
  }

  return count;
}
