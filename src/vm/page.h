#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <stdbool.h>
#include "devices/block.h"
#include "filesys/file.h"
#include "threads/palloc.h"

enum entry_type
{
  FILE_BASED,
  MEMORY_BASED
};

struct file_based
{
  struct file *f;
  off_t offset;
  size_t zero_bytes;
};

struct memory_based
{
  bool used;			/* Has this page been swapped before */
  bool swapped;			/* Is this block swapped */
  block_sector_t swap_block;	/* Swap block */
};

struct s_page_entry 
{
  enum entry_type type;		/* Type of entry */
  uint8_t *uaddr;		/* User page address (page-aligned) */
  union 
  {
    struct file_based file;
    struct memory_based memory;
  } info;				/* Attributes of entry */
  bool writing;			/* Flags that a page is being written */
  struct frame_entry *frame;	/* Frame entry if frame is allocated */
  struct hash_elem elem;	/* Entry in thread's hash table */
};

enum vm_flags
{
  VM_ZERO = PAL_ZERO             /* Zero page contents. */
};

void page_init_thread (struct thread *t);
bool page_evict (struct thread *t, uint8_t *uaddr);

#endif /* vm/page.h */
