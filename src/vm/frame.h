#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/thread.h"

struct frame_entry
{
  struct thread *t;      /* Owner process */
  uint8_t *uaddr;        /* User address in owner process */
  uint8_t *kaddr;        /* Physical address */
  /* TODO tracking information for eviction strategy */
  struct list_elem elem; /* Linked list of frame entries */
};

#endif /* vm/frame.h */
