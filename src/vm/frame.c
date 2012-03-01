#include <hash.h>
#include <string.h>
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"

static struct list_elem *clock_hand; /* The hand of the clock algorithm */
static struct list frames;     /* List of frame_entry for active frames */
static struct lock frames_lock;	/* Protects struct list frames */

static void frame_pin_no_lock (struct frame_entry *f);

/**
 * Initializes the frame table
 */
void
frame_init (void)
{
  list_init (&frames);
  lock_init (&frames_lock);
  clock_hand = list_head (&frames);
}

/**
 * Creates a frame with the given parameters. Frames begin inserted into
 * the list of frames, but pinned.
 */
static struct frame_entry *
frame_create (struct thread *t, struct s_page_entry *spe, uint8_t *kpage)
{
  /* Create entry */
  struct frame_entry *f = malloc (sizeof (struct frame_entry));
  if (f == NULL)
    PANIC ("Unable to operate on frame table");

  /* Populate fields */
  f->t = t;
  f->spe = spe;
  f->kaddr = kpage;
  f->pinned = true;

  /* Insert into list */
  lock_acquire (&frames_lock);
  list_push_back (&frames, &f->elem);
  lock_release (&frames_lock);
  
  return f;
}

/**
 * Pins a frame. Assumes frames_lock is acquired already.
 */
static void
frame_pin_no_lock (struct frame_entry *f)
{
  ASSERT (!f->pinned);
  f->pinned = true;
}

/**
 * Acquires the frames_lock and unpins a frame.
 */
void
frame_unpin (struct frame_entry *f)
{
  ASSERT (!lock_held_by_current_thread (&frames_lock));
  lock_acquire (&frames_lock);
  ASSERT (f->pinned);
  f->pinned = false;
  lock_release (&frames_lock);
}

/**
 * Helper function for the clock algorithm to treat the frame list as a
 * circularly linked list. Should not be called by others.
 */
static struct list_elem *
clock_next (void)
{
  clock_hand = list_next (clock_hand);
  if (clock_hand == list_end (&frames))
    clock_hand = list_begin (&frames);

  return clock_hand;
}

/**
 * Uses the clock algorithm to find the next frame for eviction. The
 * criteria are that the frame is untagged . After one revolution at least
 * one frame should be untagged.
 *
 * The frames_lock must be acquired before entering this method. Returns a
 * pinned frame. 
 */
static struct frame_entry *
clock_algorithm (void)
{
  struct frame_entry *f;
  struct frame_entry *clock_start;

  /* Base case: no frames */
  if (list_empty (&frames))
    return NULL;

  /* Find first unpinned frame */
  clock_start = list_entry (clock_next (), struct frame_entry, elem);
  f = clock_start;
  while (f->pinned)
  {
    f = list_entry (clock_next (), struct frame_entry, elem);
    if (f == clock_start)
      return NULL;
  }
  clock_start = f;

  /* Run clock algorithm */
  do {
    if (!f->pinned)
    {
      if (pagedir_is_accessed (f->t->pagedir, f->spe->uaddr))
	pagedir_set_accessed (f->t->pagedir, f->spe->uaddr, false);
      else
	break;
    }
    f = list_entry (clock_next (), struct frame_entry, elem);
  } while (f != clock_start);

  frame_pin_no_lock (f);

  return f;
}

/**
 * Evicts a frame from the frame table and returns it, pinned and ready to
 * use.
 */
static struct frame_entry *
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
  struct s_page_entry *spe = f->spe;

  /* Perform the eviction */
  lock_acquire (&spe->l);
  lock_release (&frames_lock);
  bool success = page_evict (f->t, f->spe);
  lock_release (&spe->l);

  /* Failure to evict */
  if (!success) 
  {
    frame_unpin (f);		/* Need to unpin */
    return NULL;
  }

  return f;
}

/**
 * Allocates a frame and marks it for the given user address. This frame
 * may come from an unallocated frame or the eviction of a
 * previously-allocated frame. The frame will be pinned.
 */
struct frame_entry*
frame_get (struct s_page_entry *spe, enum vm_flags flags)
{

  /* Attempt to allocate a brand new frame */
  uint8_t *kpage = palloc_get_page (PAL_USER | flags);

  if (kpage != NULL)
  {
    /* Make a brand new frame */
    return frame_create (thread_current (), spe, kpage);
  } else {
    /* Evict an existing frame, could be NULL */
    struct frame_entry *f = frame_evict ();
    if (f == NULL) return NULL;

    /* Associate with new thread */
    f->t = thread_current ();
    f->spe = spe;

    /* Zero out the page if requested */
    if (flags & PAL_ZERO)
      memset (f->kaddr, 0, PGSIZE); 
    return f;
  }
}

/**
 * Deallocates a frame. Returns true if frame was deallocated
 * successfully.
 */
bool
frame_free (struct frame_entry *f)
{
  lock_acquire (&frames_lock);
  if (f->pinned == false)
  {
    if (&f->elem == clock_hand)
    {
	clock_hand = list_next (clock_hand);
	list_remove (&f->elem);
	if (clock_hand == list_end (&frames))
	  clock_hand = list_begin (&frames);
    } else {
      list_remove (&f->elem);
    }
	palloc_free_page(f->kaddr);
    free (f);
  }
  lock_release (&frames_lock);

  return true;
}
