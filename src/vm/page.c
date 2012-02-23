#include <stdio.h>
#include "lib/string.h"
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"

/**
 * Hashing function to hash a struct s_page_entry by its uaddr field.
 */
static unsigned
uaddr_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct s_page_entry *spe = hash_entry (e, struct s_page_entry, elem);
  return hash_int ((int)spe->uaddr);
}

/**
 * Hashing comparison function to compare two s_page_entrys by their uaddr
 * fields.
 */
static bool
uaddr_hash_less_func (const struct hash_elem *a, const struct hash_elem *b,
		void *aux UNUSED)
{
  struct s_page_entry *lhs = hash_entry (a, struct s_page_entry, elem);
  struct s_page_entry *rhs = hash_entry (b, struct s_page_entry, elem);
  return (lhs->uaddr < rhs->uaddr);
}

/**
 * Initializes supplemental page table for a thread.
 */
void
page_init_thread (struct thread *t)
{
  hash_init (&t->s_page_table, uaddr_hash_func, uaddr_hash_less_func, NULL);
  lock_init (&t->s_page_lock);
}

/**
 * Destroys an entry in the page table (called by process_exit)
 */
void
page_destroy_thread (struct hash_elem *e, void *aux UNUSED)
{
  vm_free_page (hash_entry (e, struct s_page_entry, elem));
}

/**
 * Adds a mapping from user virtual address UPAGE to kernel virtual
 * address KPAGE to the page table.  If WRITABLE is true, the user
 * process may modify the page; otherwise, it is read-only.  UPAGE
 * must not already be mapped.  KPAGE should probably be a page
 * obtained from the user pool with palloc_get_page().  Returns true
 * on success, false if UPAGE is already mapped or if memory
 * allocation fails.
 */
static bool
install_page (struct s_page_entry *spe)
{
  ASSERT (lock_held_by_current_thread (&spe->l));
  ASSERT (spe->frame != NULL);

  void *upage = spe->uaddr;
  void *kpage = spe->frame->kaddr;
  bool writable = spe->writable;
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  ASSERT (pagedir_get_page (t->pagedir, upage) == NULL);

  bool result = pagedir_set_page (t->pagedir, upage, kpage, writable);
  frame_unpin (spe->frame);

  return result;
}

/**
 * Initializes a generic supplemental page entry. Requires further
 * specialization into a file-based or memory-based page.
 */
static struct s_page_entry *
create_s_page_entry (uint8_t *uaddr, bool writable)
{
  struct s_page_entry *spe = malloc (sizeof (struct s_page_entry));
  if (spe == NULL)
    return NULL;

  /* Page align the address */
  uaddr = (uint8_t*)pg_round_down (uaddr);

  /* Set fields */
  struct thread *t = thread_current ();
  spe->uaddr = uaddr;
  spe->writable = writable;
  spe->frame = NULL;
  lock_init (&spe->l);
	
  /* Install into hash table */
  lock_acquire (&t->s_page_lock);
  hash_insert (&t->s_page_table, &spe->elem);
  lock_release (&t->s_page_lock);

  return spe;
}

/**
 * Adds a memory-based supplemental page table entry to the current
 * process.
 */
bool
vm_add_memory_page (uint8_t *uaddr, bool writable)
{
  ASSERT (uaddr < PHYS_BASE);
  struct s_page_entry *spe = create_s_page_entry (uaddr, writable);
  if (spe == NULL)
    return false;

  spe->type = MEMORY_BASED;
  spe->info.memory.swapped = true;
  spe->info.memory.used = false;

  return true;
}

/**
 * Constructs a file-based supplemental page table entry.
 */
static bool
add_file_page (uint8_t *uaddr, struct file *f, off_t offset, 
		  size_t zero_bytes, bool writable, bool init_only)
{
  ASSERT (uaddr < PHYS_BASE);
  struct s_page_entry *spe = create_s_page_entry (uaddr, writable);
  if (spe == NULL)
    return false;

  spe->type = FILE_BASED;
  spe->info.file.f = f;
  spe->info.file.offset = offset;
  spe->info.file.zero_bytes = zero_bytes;
  spe->info.file.init_only = init_only;

  return true;
}

/**
 * Adds a file-based supplemental page table entry to the current process
 * that is paged into and out of the original file..
 */
bool
vm_add_file_page (uint8_t *uaddr, struct file *f, off_t offset,
		  size_t zero_bytes, bool writable)
{
  return add_file_page (uaddr, f, offset, zero_bytes, writable, false);
}

/**
 * Adds a file-based supplemental page table entry to the current process
 * that can be modified but is not written to disk, but rather later
 * becomes a memory-based page.
 */
bool
vm_add_file_init_page (uint8_t *uaddr, struct file *f, off_t offset,
		       size_t zero_bytes) 
{
  return add_file_page (uaddr, f, offset, zero_bytes, true, true);
}

/**
 * Frees a supplemental page entry and removes it from the current
 * process.
 */
bool
vm_free_page (struct s_page_entry *spe)
{
  lock_acquire (&spe->l);
  /* TODO handle writing out files and/or freeing up swap */

  frame_free (spe);		/* Free frame */
  lock_release (&spe->l);
  free (spe);			/* Free s_page_entry */

  return true;
}

/**
 * Swaps a page to disk. Does not clear the frame.
 */
static bool
page_swap (struct s_page_entry *spe)
{
  ASSERT (spe->type == MEMORY_BASED);
  /* Only swap if page has been used at some point */
  ASSERT (!spe->info.memory.swapped);
  ASSERT (lock_held_by_current_thread (&spe->l));

  struct thread *t = spe->frame->t;
  bool write_needed = spe->info.memory.used || pagedir_is_dirty
    (t->pagedir, spe->uaddr);

  if (write_needed)
  {
    lock_acquire (&fd_all_lock);
    swap_write (spe->frame->kaddr, &spe->info.memory.swap_begin);
    lock_release (&fd_all_lock);
    spe->info.memory.used = true;
  } 

  spe->info.memory.swapped = true;

  return true;
}

/**
 * Unswaps a page into a frame.
 */
static bool
page_unswap (struct s_page_entry *spe)
{
  ASSERT (spe->info.memory.swapped);
  ASSERT (lock_held_by_current_thread (&spe->l));

  if (spe->info.memory.used)
  {
    /* Fetch from swap */
    spe->frame = frame_get (spe->uaddr, 0);
    if (!spe->frame) return false;

    lock_acquire (&fd_all_lock);
    bool success = swap_load (spe->frame->kaddr,
			      spe->info.memory.swap_begin);
    lock_release (&fd_all_lock);
    if (!success)
    {
      frame_unpin (spe->frame);
      return false;
    }
  } else {
    /* Brand new page, just allocate it */
    spe->frame = frame_get (spe->uaddr, PAL_ZERO);
    if (!spe->frame) return false;

    spe->info.memory.used = true;
  }

  spe->info.memory.swapped = false;

  /* Install the page into the page table */
  install_page (spe);

  return true;
}

/**
 * Writes a FILE_BASED page to its file. Does not free the frame
 */
static bool
page_file (struct s_page_entry *spe) 
{
  ASSERT (spe != NULL);
  ASSERT (spe->type == FILE_BASED);
  ASSERT (lock_held_by_current_thread (&spe->l));

  struct frame_entry *frame = spe->frame;
  ASSERT (frame != NULL);
  struct file_based *info = &spe->info.file;

  ASSERT (info->f != NULL);

  // TODO Think about all the race conditions ... 
  /* Unmap the file from the thread before it is written */
  struct thread *t = spe->frame->t;

  /* Check if we need to write at all */  
  lock_acquire (&t->s_page_lock);
  bool write_needed = spe->writable && pagedir_is_dirty (t->pagedir,
      spe->uaddr);
  lock_release (&t->s_page_lock);

  bool result = false;
  if(write_needed)
  {
    /* Write the file out to disk */
    lock_acquire(&fd_all_lock);
    file_seek (info->f, info->offset);
    size_t bytes_write = PGSIZE - info->zero_bytes;
    bool num_written = file_write (info->f, frame->kaddr, bytes_write);
    lock_release(&fd_all_lock);

    result = bytes_write == num_written;
  } else {
    result = true;
  }

  return result;
}

/**
 * Reads a file-based page back from disk into a frame.
 */
static bool
page_unfile (struct s_page_entry *spe)
{
  ASSERT (spe != NULL);
  ASSERT (lock_held_by_current_thread (&spe->l));

  struct frame_entry *frame = frame_get (spe->uaddr, 0);
  if (frame == NULL) return false;

  struct file_based *info = &spe->info.file;

  ASSERT (info->f != NULL);

  lock_acquire(&fd_all_lock);

  /* Read page into memory */
  file_seek (info->f, info->offset);
  int target_bytes = PGSIZE - info->zero_bytes;
  int bytes_read = file_read (info->f, frame->kaddr, target_bytes);
  lock_release(&fd_all_lock);   

  spe->frame = frame;
  if (bytes_read != target_bytes) 
  {
    free (spe);
    return false;
  }
  memset (frame->kaddr + bytes_read, 0, info->zero_bytes);

  /* If this page was only initialization, transform it into a memory
   * page */
  if (spe->info.file.init_only) 
  {
    spe->type = MEMORY_BASED;
    spe->info.memory.used = true;
    spe->info.memory.swapped = false;
  }

  /* Install the page into the page table */
  install_page (spe);

  return true;
}

/**
 * Evicts the page belonging to thread t associated with the given
 * uaddr. Assumes the frame associated with it is pinned.
 */
bool
page_evict (struct thread *t, uint8_t *uaddr)
{
  struct s_page_entry key = {.uaddr = uaddr};
  struct s_page_entry *spe = NULL;

  /* Look up supplemental page entry */
  lock_acquire (&t->s_page_lock);
  struct hash_elem *e = hash_find (&t->s_page_table, &key.elem);
  if (e == NULL) 
  {
    lock_release (&t->s_page_lock);
    return false;
  }
  spe = hash_entry (e, struct s_page_entry, elem);
  lock_acquire (&spe->l);
  lock_release (&t->s_page_lock);

  /* Lock on this supplemental page entry */
  pagedir_clear_page (t->pagedir, spe->uaddr);

  /* Perform eviction */
  bool result = false;
  switch (spe->type)
  {
  case FILE_BASED:
    result = page_file (spe);
    break;
  case MEMORY_BASED:
    result = page_swap (spe);
    break;
  default:
    PANIC ("Unknown page type!");
  }

  if (!result)
    PANIC ("Failure during eviction");

  spe->frame = NULL;
  lock_release (&spe->l);

  return result;
}

/**
 * Attempts to load a page using the supplemental page table.
 */
bool
page_load (uint8_t *fault_addr)
{
  ASSERT (fault_addr < PHYS_BASE);

  /* Look up the supplemental page entry */
  struct thread *t = thread_current ();
  uint8_t* uaddr = (uint8_t*)pg_round_down (fault_addr);
  struct s_page_entry key = {.uaddr = uaddr};

  lock_acquire (&t->s_page_lock);
  struct hash_elem *e = hash_find (&t->s_page_table, &key.elem);
  if (e == NULL) 
  {
    lock_release (&t->s_page_lock);
    return false;
  }

  /* Lock on this supplemental page entry */
  struct s_page_entry *spe = hash_entry (e, struct s_page_entry, elem);
  lock_acquire (&spe->l);
  lock_release (&t->s_page_lock);

  /* Load the page */
  bool result = false;
  switch (spe->type)
  {
  case FILE_BASED:
    result = page_unfile (spe);
    break;
  case MEMORY_BASED:
    result = page_unswap (spe);
    break;
  default:
    PANIC ("Unknown page type!");
  }

  lock_release (&spe->l);

  return result;
}
