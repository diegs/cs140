#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/thread.h"
#include "threads/synch.h"
#include "vm/page.h"

struct frame_entry
{
  struct thread *t;  /* Owner process */
  uint8_t *uaddr;    /* User address in owner process */
  uint8_t *kaddr;    /* Physical address */
  struct list_elem elem;	/* Linked list of frame entries */
  bool pinned;			/* Whether this frame is pinned or not */
};

void frame_init (void);
struct frame_entry *frame_get (uint8_t *uaddr, enum vm_flags flags);
bool frame_free (struct frame_entry *f);
void frame_install (struct frame_entry *f);
void frame_unpin (struct frame_entry *f);

#endif /* vm/frame.h */
