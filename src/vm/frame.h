#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/thread.h"
#include "threads/synch.h"
#include "vm/page.h"

struct frame_entry
{
  struct thread *t;		/* Owner thread */
  struct s_page_entry *spe;	/* Owner page entry */
  uint8_t *kaddr;		/* Physical address */
  struct list_elem elem;	/* Linked list of frame entries */
  bool pinned;			/* Whether this frame is pinned or not */
  struct condition unpinned;	/* Condition variable to signal when a
				   frame is unpinned */
};

void frame_init (void);
struct frame_entry *frame_get (struct s_page_entry *spe, enum vm_flags flags);
bool frame_free (struct frame_entry *f);
void frame_install (struct frame_entry *f);
void frame_unpin (struct frame_entry *f);
void frame_destroy_thread (void);
void thread_pin_frames (struct thread *t);

#endif /* vm/frame.h */
