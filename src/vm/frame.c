#include "threads/malloc.h"
#include "vm/frame.h"
#include "vm/page.h"

static struct list frames;            /* List of frame_entry for active frames */
static struct lock frames_lock;       /* Protects struct list frames */

/**
 * Initializes the frame table
 */
void
frame_init (void)
{
  list_init (&frames);
  lock_init (&frames_lock);
}

/**
 * Inserts an entry for a page belonging to a thread into the frame table
 */
static void
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
}

/**
 * Evicts a frame from the frame table and performs the necessary
 * actions. Returns the kernel address of the free'd frame.
 */
static uint8_t *
frame_evict (void)
{
  uint8_t *kpage;

  /* Check if we have a frame to evict */
  lock_acquire (&frames_lock);
  if (list_empty (&frames))
  {
    lock_release (&frames_lock);
    return NULL;
  }

  /* Choose a frame to evict */
  /* TODO eviction algorithm */
  struct frame_entry *f = list_entry (list_pop_front (&frames),
				      struct frame_entry,
				      elem);
  lock_release (&frames_lock);

  /* Perform the eviction */
  bool success = page_evict (f->t, f->uaddr);

  if (!success)
  {
    /* Failed to evict -- reinsert into frame table (makes a new entry) */
    frame_insert (f->t, f->uaddr, f->kaddr);
    kpage = NULL;
  } else {
    /* This kernel address is now free */
    kpage = f->kaddr;
  }
  
  /* Reclaim frame entry */
  free (f);
  return kpage;
}

/**
 * Allocates a frame and marks it for the given user address. This frame
 * may come from an unallocated frame or the eviction of a
 * previously-allocated frame.
 */
uint8_t*
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
  frame_insert (thread_current (), uaddr, kpage);

  return kpage;
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
    palloc_free_page (&f->kaddr);
    list_remove (&f->elem);
    free (f);
    spe->frame = NULL;	/* For safety */
  } 
  lock_release (&frames_lock);

  return true;
}
