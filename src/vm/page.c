#include "threads/palloc.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "vm/page.h"

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}


uint8_t *
vm_add_page (uint8_t *uaddr, bool writable, enum vm_flags flags)
{
  /* allocate a page */
  // TODO eviction logic goes here
  uint8_t *kpage = palloc_get_page (PAL_USER | flags);
  if (kpage == NULL)
    return NULL;
  
  /* install page */
  // TODO supplemental page table logic goes here
  // TODO update frame table here
  bool success = install_page (uaddr, kpage, writable);
  if (!success)
  {
    palloc_free_page (kpage);
    return NULL;
  }

  return kpage;
}

bool
vm_free_page (uint8_t *uaddr)
{
  struct thread *t = thread_current ();

  /* free page */
  // TODO update frame table here
  uint8_t *kpage = pagedir_get_page (t->pagedir, uaddr);
  if (kpage == NULL)
    return false;
  palloc_free_page (kpage);

  /* remove from directory */
  // TODO supplemental page logic goes here
  pagedir_clear_page (t->pagedir, uaddr);
  return true;
}
