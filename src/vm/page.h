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
  bool unused;
  block_sector_t swap_block;
};

struct s_page_entry 
{
  enum entry_type type;  /* Type of entry */
  union 
  {
    struct file_based;
    struct memory_based;
  } info;                /* Attributes of entry */
  bool writing;          /* Flags that a page is being written */
  struct hash_elem elem; /* Entry in thread's hash table */
};

enum vm_flags
{
  VM_ZERO = PAL_ZERO             /* Zero page contents. */
};

uint8_t * vm_add_page (uint8_t *uaddr, bool writable, enum vm_flags flags);
bool vm_free_page (uint8_t *uaddr);

#endif /* vm/page.h */
