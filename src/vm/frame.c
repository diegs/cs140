#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"

static struct list_elem *clock_hand;  /* The hand of the clock algorithm */
static struct list frames;     /* List of frame_entry for active frames */
static struct lock frames_lock;	/* Protects struct list frames */

/**
 * Initializes the frame table
 */
void
frame_init (void)
{
  list_init (&frames);
  lock_init (&frames_lock);
  clock_hand = list_end (&frames);
}

/**
 * Inserts an entry for a page belonging to a thread into the frame table
 */
static struct frame_entry *
frame_insert (struct thread *t, uint8_t *uaddr, uint8_t *kpage)
{
  /* Create entry */
  struct frame_entry *f = malloc (sizeof (struct frame_entry));
  if (f == NULL)
    PANIC ("Unable to operate on frame table");

  /* Populate fields */
  f->t = t;
  f->uaddr = uaddr;
  f->kaddr = kpage;

  /* Insert into list */
  lock_acquire (&frames_lock);
  list_push_back (&frames, &f->elem);
  lock_release (&frames_lock);
  
  return f;
}

/* Treats the list as a circularly linked list */
static struct list_elem *
clock_next (void)
{
  clock_hand = list_next (clock_hand);
  if (clock_hand == list_end (&frames))
    clock_hand = list_begin (&frames);

  return clock_hand;
}

/* Magic algorithm: find the next frame that is untagged */
static struct frame_entry *
clock_algorithm (void)
{
  struct frame_entry *f;
  struct frame_entry *clock_start = list_entry (clock_hand, struct
						frame_entry, elem);

  if (list_empty (&frames))
    return NULL;

  while ((f = list_entry (clock_next (), struct frame_entry, elem)) !=
	 clock_start)
  {
    if (pagedir_is_accessed (f->t->pagedir, f->uaddr))
      pagedir_set_accessed (f->t->pagedir, f->uaddr, false);
    else
      break;
  }

  return f;
}

/**
 * Evicts a frame from the frame table and reassigns it to the 
 * given user address. 
 */
static struct uint8_t*
frame_evict (void)
{
  /* Choose a frame to evict */
  lock_acquire (&frames_lock);
  struct frame_entry *f = clock_algorithm ();
  if (f == NULL) 
  {
    lock_release (&frames_lock);
    return NULL;	/* Could not find a frame to evict */
  }
  list_remove (&f->elem);
  lock_release (&frames_lock);

  /* Perform the eviction */
  bool success = page_evict (f->t, f->uaddr);
  
  /* Put the frame back if we could not evict it */
  if (!success) 
  {
    lock_acquire (&frames_lock);
    list_push_back (&frames, &f->elem);
    lock_release (&frames_lock);
    return NULL;
  }

  /* Clean up frame memory */
  uint8_t *kaddr = f->kaddr;
  free (f);
  
  return kaddr;
}

/**
 * Allocates a frame and marks it for the given user address. This frame
 * may come from an unallocated frame or the eviction of a
 * previously-allocated frame.
 */
struct frame_entry*
frame_get (uint8_t *uaddr, enum vm_flags flags)
{
  /* Attempt to allocate a brand new frame */
  uint8_t *kpage = palloc_get_page (PAL_USER | flags);

  /* Evict an existing frame */
  if (kpage == NULL)
    kpage = frame_evict ();

  /* Failed to evict */
  if (kpage == NULL)
    return NULL;

  /* Make a new frame table entry */
  return frame_insert (thread_current (), uaddr, kpage);
}

/**
 * Deallocates a frame. Returns true if frame was dealloacted successfully.
 */
bool
frame_free (struct s_page_entry *spe)
{
  lock_acquire (&frames_lock);
  if (spe->frame != NULL)
  {
    struct frame_entry *f = spe->frame;
    palloc_free_page (f->kaddr);
    if (&f->elem == clock_hand)
    {
      clock_hand = list_next (clock_hand);
      list_remove (&f->elem);
      if (clock_hand == list_end (&frames))
        clock_hand = list_begin (&frames);
    } else {
      list_remove (&f->elem);
    }

    free (f);
    spe->frame = NULL;	/* For safety */
  } 
  lock_release (&frames_lock);

  return true;
}
