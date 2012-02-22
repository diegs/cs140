#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <stdbool.h>
#include "devices/block.h"
#include "filesys/file.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"

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
  bool init_only;
};

struct memory_based
{
  bool used;			/* Has this page been swapped before */
  bool swapped;			/* Is this block swapped */
  block_sector_t swap_blocks[PGSIZE/BLOCK_SECTOR_SIZE];	/* Swap block */
};

struct s_page_entry 
{
  enum entry_type type;		/* Type of entry */
  uint8_t *uaddr;		/* User page address (page-aligned) */
  bool writable;		/* Whether page is writable */
  union 
  {
    struct file_based file;
    struct memory_based memory;
  } info;				/* Attributes of entry */
  struct frame_entry *frame;	/* Frame entry if frame is allocated */
  struct hash_elem elem;	/* Entry in thread's hash table */
};

enum vm_flags
{
  VM_ZERO = PAL_ZERO             /* Zero page contents. */
};

bool vm_add_memory_page (uint8_t *uaddr, bool writable);
bool vm_add_file_page (uint8_t *uaddr, struct file *f,
		       off_t offset, size_t zero_bytes, bool writable);
bool vm_add_file_init_page (uint8_t *uaddr, struct file *f, off_t
          offset, size_t zero_bytes);
bool vm_free_page (uint8_t *uaddr);
void page_init_thread (struct thread *t);
void page_destroy_thread (struct hash_elem *e, void *aux UNUSED);
bool page_evict (struct thread *t, uint8_t *uaddr);
bool page_load (uint8_t *fault_addr);
bool vm_add_file_page (uint8_t *uaddr, struct file *f, off_t offset,
		       size_t zero_bytes, bool writable);
bool vm_add_file_init_page (uint8_t *uaddr, struct file *f, off_t offset,
			    size_t zero_bytes);
#endif /* vm/page.h */
