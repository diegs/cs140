#include "vm/frame.h"

static struct list frames;
static struct lock frames_lock;

void
frame_init (void)
{
  list_init (&frames);
  lock_init (&frames_lock);
}

static void
frame_insert (struct thread *t, uint8_t *uaddr, uint8_t *kpage)
{
  struct frame_entry *f = malloc (sizeof (struct frame_entry));
  if (frame_entry == NULL)
    PANIC ("Unable to operate on frame table");

  lock_acquire (&frames_lock);
  list_push_back (&frames, &f->elem);
  lock_release (&frames_lock);
}

static uint8_t *
frame_evict (void)
{
  uint8_t *kpage = NULL;

  /* Check if we have a frame to evict */
  if (list_empty (&frames))
    return kpage;

  /* Choose a frame to evict */
  /* TODO eviction algorithm */
  lock_acquire (&frames_lock);
  struct frame_entry *f = list_entry (list_pop_front (&frames),
				      struct frame_entry,
				      elem);
  lock_release (&frames_lock);

  /* Perform the eviction */
  bool success = page_evict (f->t, f->uaddr);

  if (!success)
    /* Failed to evict -- reinsert into frame table */
    frame_insert (f->t, f->uaddr, f->kpage);
  else
    /* This kernel address is free */
    kpage = f->kpage;
  
  free (f); /* Reclaim frame_entry */
  return kpage;
}

uint8_t *
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

  /* Make a new frame */
  frame_insert (thread_current (), uaddr, kpage);

  return kpage;
}
