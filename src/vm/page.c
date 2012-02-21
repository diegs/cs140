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

static unsigned
uaddr_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct s_page_entry *spe = hash_entry (e, struct s_page_entry, elem);
  return hash_int ((int)spe->uaddr);
}

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
  cond_init (&t->s_page_cond);
}

/**
 * Destroys an entry in the page table (called by process_exit) */
void
page_destroy_thread (struct hash_elem *e, void *aux UNUSED)
{
  struct s_page_entry *spe = hash_entry (e, struct s_page_entry, elem);
  /* We do not need to free the allocated page because it will be freed by
     the page directory on thread destruction ASSUMPTION: this destroy
     function should only be called when the thread is being destroyed. */
  if (spe->frame != NULL) 
    free (spe->frame); 
  free (spe);
}

/**
 * Adds a mapping from user virtual address UPAGE to kernel virtual
 * address KPAGE to the page table.  If WRITABLE is true, the user process
 * may modify the page; otherwise, it is read-only.  UPAGE must not
 * already be mapped.  KPAGE should probably be a page obtained from the
 * user pool with palloc_get_page().  Returns true on success, false if
 * UPAGE is already mapped or if memory allocation fails.
 */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  if (pagedir_get_page (t->pagedir, upage) != NULL)
  {
    pagedir_clear_page (t->pagedir, upage);
  }
  return pagedir_set_page (t->pagedir, upage, kpage, writable);
}

/* Finds a page entry in a context where the given thread's page 
   table lock has been acquired.
   */
static struct s_page_entry *
safe_get_page_entry (struct thread *t, uint8_t *uaddr) 
{
  struct s_page_entry key = {.uaddr = uaddr};
  struct s_page_entry *spe = NULL;

  struct hash_elem *e = hash_find (&t->s_page_table, &key.elem);
  if (e != NULL)
    spe = hash_entry (e, struct s_page_entry, elem);

  return spe;
}

/* Initializes a supplemental page entry */
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
  spe->writing = false;
  spe->frame = NULL;
  spe->t = t;
  lock_init(&spe->lock);
	
  /* Install into hash table */
  lock_acquire (&t->s_page_lock);
  hash_insert (&t->s_page_table, &spe->elem);
  lock_release (&t->s_page_lock);

  return spe;
}

/**
 * Adds a memory-based page to the current process
 */
bool
vm_add_memory_page (uint8_t *uaddr, bool writable)
{
  struct s_page_entry *spe = create_s_page_entry (uaddr, writable);
  if (spe == NULL)
    return false;

  spe->type = MEMORY_BASED;
  spe->info.memory.swapped = true;
  spe->info.memory.used = false;

  return true;
}

/**
 * Adds a file-based page to the current process
 */
bool
vm_add_file_page (uint8_t *uaddr, struct file *f, off_t offset,
		  size_t zero_bytes, bool writable)
{
  struct s_page_entry *spe = create_s_page_entry (uaddr, writable);
  if (spe == NULL)
    return false;

  spe->type = FILE_BASED;
  spe->info.file.f = f;
  spe->info.file.offset = offset;
  spe->info.file.zero_bytes = zero_bytes;

  return true;
}

bool
vm_free_page (uint8_t *uaddr)
{
  struct thread *t = thread_current ();
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
  hash_delete (&t->s_page_table, &spe->elem);
  pagedir_clear_page (t->pagedir, uaddr); /* No longer valid for page fault */
  lock_release (&t->s_page_lock);

  /* Free frame if necessary */
  frame_free (spe);

  return true;
}

/* Swaps a page to disk. Does not clear the frame. */
static bool
page_swap (struct s_page_entry *spe)
{
  ASSERT (spe->type == MEMORY_BASED);
  lock_acquire(&spe->lock);
  ASSERT (!spe->info.memory.swapped);
  /* Only swap if page has been used at some point */
  if (spe->info.memory.used || pagedir_is_dirty (spe->t, spe->uaddr))
  {
    swap_write (spe->uaddr, spe->info.memory.swap_blocks);
    spe->info.memory.used = true;
  } 

  spe->info.memory.swapped = true;
  pagedir_clear_page (spe->t->pagedir, spe->uaddr);
  lock_release(&spe->lock);
  return true;
}

/* Unswaps a page into a frame */
static bool
page_unswap (struct s_page_entry *spe)
{
  lock_acquire(&spe->lock);
	
  ASSERT(spe->info.memory.swapped);

  if (spe->info.memory.used)
  {
    /* Fetch from swap */
    spe->frame = frame_get (spe->uaddr, 0);
    if (!spe->frame)
    {
      lock_release(&spe->lock);
      return false;
    }
    bool success = swap_load (spe->frame->kaddr, spe->info.memory.swap_blocks);
    if (!success)
    {
      frame_free (spe);
      lock_release(&spe->lock);
      return false;
    }
  } else {
    /* Brand new page, just allocate it */
    spe->frame = frame_get (spe->uaddr, PAL_ZERO);
    if (!spe->frame)
    {
      lock_release(&spe->lock);
      return false;
    }
    spe->info.memory.used = true;
  }

  spe->info.memory.swapped = false;
  install_page (spe->uaddr, spe->frame->kaddr, spe->writable);
  lock_release(&spe->lock);
  return true;
}


/* Writes a FILE_BASED page to its file. Does not free the frame */
static bool
page_file (struct s_page_entry *spe) 
{
  ASSERT (spe != NULL);
  ASSERT (spe->type == FILE_BASED);

  struct frame_entry *frame = spe->frame;
  ASSERT(frame != NULL);
  struct file_based *info = &spe->info.file;

  ASSERT (info->f != NULL);

  // TODO Think about all the race conditions ... 
  /* Unmap the file from the thread before it is written */
  struct thread *t = spe->t;
  lock_acquire (&t->s_page_lock);

  pagedir_clear_page (t->pagedir, spe->uaddr);

  /* Check if we need to write at all */  
  if (!spe->writable || !pagedir_is_dirty(t->pagedir, spe->uaddr)) 
  {
    lock_release (&t->s_page_lock);
    return true;
  }

  spe->writing = true;
  lock_release (&t->s_page_lock);


  /* Write the file out to disk */
  lock_acquire(&fd_all_lock);
  file_seek (info->f, info->offset);
  size_t bytes_write = PGSIZE - info->zero_bytes;
  bool num_written = file_write (info->f, frame->kaddr, bytes_write);
  lock_release(&fd_all_lock);

  lock_acquire (&t->s_page_lock);
  spe->writing = false;
  cond_signal (&t->s_page_cond, &t->s_page_lock);
  lock_release (&t->s_page_lock);

  return bytes_write == num_written;
}

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
  lock_release (&t->s_page_lock);

  /* Perform eviction */
  switch (spe->type)
  {
  case FILE_BASED:
    return page_file (spe);
    break;
  case MEMORY_BASED:
    return page_swap (spe);
    break;
  default:
    PANIC ("Unknown page type!");
  }

  return false;
}

static bool
page_unfile (struct s_page_entry *spe)
{
  ASSERT (spe != NULL);

  struct frame_entry *frame = frame_get (spe->uaddr, 0);
  struct file_based *info = &spe->info.file;

  ASSERT (info->f != NULL);
  
  lock_acquire(&fd_all_lock);
  /* Read page into memory */
  file_seek (info->f, info->offset);

  size_t bytes_read = PGSIZE - info->zero_bytes;
  if (file_read (info->f, frame->kaddr, bytes_read) != bytes_read) 
  { 
    lock_release(&fd_all_lock);   
    return false;
  }
  lock_release(&fd_all_lock);
  memset (frame->kaddr + bytes_read, 0, info->zero_bytes);

  /* If this was writable transform it into a memory page */
  if (spe->writable) 
  {
    spe->type = MEMORY_BASED;
    spe->info.memory.used = true;
    spe->info.memory.swapped = false;
  }

  /* Update the supplementary page table entry */
  struct thread *t = thread_current ();
  lock_acquire (&t->s_page_lock);
  spe->frame = frame;
  install_page (frame->uaddr, frame->kaddr, spe->writable);
  lock_release (&t->s_page_lock);

  return true;
}

/**
 * Attempts to load a page using the supplemental page table.
 */
bool
page_load (uint8_t *fault_addr)
{
  /* Look up the supplemental page entry */
  struct thread *t = thread_current ();
  uint8_t* uaddr = (uint8_t*)pg_round_down (fault_addr);
  struct s_page_entry key = {.uaddr = uaddr};

  lock_acquire (&t->s_page_lock);
  struct hash_elem *e = hash_find (&t->s_page_table, &key.elem);
  if (e == NULL)
  {
    /* Not a valid page */
    lock_release (&t->s_page_lock);
    return false;
  }

  struct s_page_entry *spe = hash_entry (e, struct s_page_entry, elem);
  lock_release (&t->s_page_lock);

  /* Load the page */
  switch (spe->type)
  {
  case FILE_BASED:
    return page_unfile(spe);
    break;
  case MEMORY_BASED:
    return page_unswap (spe);
    break;
  default:
    PANIC ("Unknown page type!");
  }

  return false;
}
