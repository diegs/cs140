#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/thread.h"
#include "vm/page.h"

struct frame_entry
{
  struct thread *t;  /* Owner process */
  uint8_t *uaddr;    /* User address in owner process */
  uint8_t *kaddr;    /* Physical address */
  struct list_elem elem;	/* Linked list of frame entries */
};

void frame_init (void);
struct frame_entry *frame_get (uint8_t *uaddr, enum vm_flags flags);
bool frame_free (struct s_page_entry *spe);

#endif /* vm/frame.h */
