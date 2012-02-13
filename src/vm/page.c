#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"

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
  frame_free (spe);
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
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
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
  spe->uaddr = uaddr;
  spe->writable = writable;
  spe->writing = false;
  spe->frame = NULL;

  /* Install into hash table */
  struct thread *t = thread_current ();
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
  spe->info.memory.swapped = false;
  spe->info.memory.swap_block = 0;
  spe->info.memory.used = false;

  return true;
}

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
  free (spe);

  return true;
}

/* TODO implement */
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
    /* TODO handle file eviction */
    break;
  case MEMORY_BASED:
    /* TODO handle memory eviction */
    break;
  default:
    PANIC ("Unknown page type!");
  }

  return true;
}

static bool
page_file (void)
{
  // TODO peter
  return false;
}

static bool
page_swap (void)
{
  // TODO diego
  return false;
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
    /* TODO handle file eviction */
    return page_file();
    break;
  case MEMORY_BASED:
    return page_swap();
    break;
  default:
    PANIC ("Unknown page type!");
  }

  return false;
}
