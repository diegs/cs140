#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "devices/timer.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of processes in the THREAD_READY state for MLFQS scheduling */
static struct list mlfqs_lists[PRI_MAX+1];

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* MLFQS scheduling variables */
static fp_t load_avg;
static int mlfqs_num_ready = 0;

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int
    priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static void mlfqs_scheduler_tick (void);

static int clamp (int val, int min, int max);

static int 
clamp (int val, int min, int max)
{
  if (val < min) return min;
  if (val > max) return max;
  return val;
}

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  if (thread_mlfqs)
  {
    /* MLFQS initialization */
    int i;
    for (i = 0; i < PRI_MAX + 1; i++)
      list_init (&mlfqs_lists[i]);

    /* Prepare MLFQS global variables */
    load_avg = int2fp (0);
  }
  
  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

#ifdef USERPROG
  list_init (&initial_thread->p_children);   /* List of child processes */
#endif
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

static int
compute_mlfqs_priority (struct thread *t) 
{
  fp_t recent_contrib = fpdiv (t->recent_cpu, int2fp (4));
  fp_t nice_contrib = fpmul (int2fp (t->nice), int2fp (2));
  int new_priority = PRI_MAX - fp2int (recent_contrib) 
                      - fp2int (nice_contrib);
  return clamp (new_priority, PRI_MIN, PRI_MAX);
}

static void
set_mlfqs_priority (struct thread *t, int new_priority) 
{
  /* Do not allow the idle thread mlfqs_priority to change */
  if (!idle_thread || t == idle_thread) return;

  t->mlfqs_priority = clamp (new_priority, PRI_MIN, PRI_MAX);
  t->effective_priority = t->mlfqs_priority;

  /* Move the element to the list corresponding to its new
     priority if it is in a ready queue */
  if (t->status == THREAD_READY) 
  {
    enum intr_level old_level = intr_disable ();
    list_remove (&t->mlfqs_elem);
    list_push_back (&mlfqs_lists[t->mlfqs_priority], &t->mlfqs_elem);
    intr_set_level (old_level);
  }

}

static void
mlfqs_action_update_thread_priority (struct thread *t, void *aux UNUSED)
{
  set_mlfqs_priority (t, compute_mlfqs_priority (t));
}

static void
mlfqs_action_update_recent_cpu (struct thread *t, void *aux UNUSED)
{
  fp_t numerator = fpmul (int2fp (2), load_avg);
  fp_t denominator = fpadd (numerator, int2fp (1));
  fp_t coeff = fpdiv (numerator, denominator);
  t->recent_cpu = fpadd (fpmul (coeff, t->recent_cpu), 
                          int2fp (t->nice));
}

static void
mlfqs_scheduler_tick (void)
{
  struct thread *cur_thread = thread_current ();

  /* Update MLFQS: recent_cpu for the current thread */
  if (cur_thread != idle_thread) 
  {
    cur_thread->recent_cpu = 
      fpadd (cur_thread->recent_cpu, int2fp (1));
  }

  /* Update MLFQS: load average and recent CPU, once every second */
  if (timer_ticks () % TIMER_FREQ == 0)
  {
    /* Update load_avg */
    int current_load = mlfqs_num_ready;
    if (cur_thread != idle_thread) current_load++;
    load_avg = fpadd(fpmul (fpdiv (int2fp(59), int2fp (60)), load_avg),
		     fpdiv (int2fp (current_load), int2fp (60)));

    /* Update recent_cpu for all threads */
    thread_foreach (mlfqs_action_update_recent_cpu, NULL);

    /* Update priority for all threads */
    thread_foreach (mlfqs_action_update_thread_priority, NULL);
  } else if (timer_ticks () % TIME_SLICE == 0) {
    /* Theoretically we should update all priorities every 4 ticks, but
       the only thing that's changed is our ticks so only update us */
    mlfqs_action_update_thread_priority (thread_current (), NULL);
  }
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *cur_thread = thread_current ();

  /* Update statistics. */
  if (cur_thread == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (cur_thread->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (thread_mlfqs) mlfqs_scheduler_tick ();

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Handle MLFQS initialization specific to threads making other
     threads */
  struct thread *cur_thread = thread_current ();
  t->nice = clamp (cur_thread->nice, NICE_MIN, NICE_MAX);
  t->recent_cpu = cur_thread->recent_cpu;
  t->mlfqs_priority = cur_thread->mlfqs_priority;

#ifdef USERPROG
  /* Allocate PCB */
  t->p_status = palloc_get_page (0);
  if (t->p_status == NULL)
  {
    palloc_free_page (t);
    return TID_ERROR;
  }

  /* Initialize PCB */
  list_init (&t->p_children);   /* List of child processes */
  t->p_status->tid = t->tid;
  t->p_status->status = PROCESS_RUNNING;
  t->p_status->parent_alive = true;
  lock_init (&t->p_status->l);
  cond_init (&t->p_status->cond);

  /* Link new thread's PCB up to its parent thread */
  list_push_back (&thread_current ()->p_children, &t->p_status->elem);
#endif

  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

  /* Add to run queue. */
  thread_unblock (t);

  /* Run immediately if higher priority */
  if (t->effective_priority > thread_get_priority ())
    thread_yield ();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

bool
cmp_thread_priority(const struct list_elem *a,
  const struct list_elem *b, void* aux UNUSED) 
{
  const struct thread *a_thread = list_entry(a, struct thread, elem);
  const struct thread *b_thread = list_entry(b, struct thread, elem);

  return a_thread->effective_priority < b_thread->effective_priority;
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  
  if (!thread_mlfqs) 
  {
    /* Add to the standard scheduling list */
    list_push_back (&ready_list, &t->elem);
  } else {
    /* Add to the MLFQS scheduling list */
    list_push_back (&mlfqs_lists[t->mlfqs_priority], &t->mlfqs_elem);
    mlfqs_num_ready++;
  }

  t->status = THREAD_READY;

  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) {
    if (!thread_mlfqs) list_push_back (&ready_list, &cur->elem);
    else 
    {
      list_push_back (&mlfqs_lists[cur->mlfqs_priority], &cur->mlfqs_elem);
      mlfqs_num_ready++;
    }
  }

  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Returns the highest priority in the ready thread list, or PRI_MIN if
   the list is empty. Requires interrupts to be disabled. */
static int
highest_thread_priority (void) 
{
  if (list_empty (&ready_list)) return PRI_MIN;
  struct thread *t = list_entry (list_max (&ready_list,
					   cmp_thread_priority, NULL),
				 struct thread, elem);
  return t->priority;
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  enum intr_level old_level = intr_disable ();
  struct thread *t = thread_current ();
  t->priority = new_priority;
  int new_effective_priority = thread_get_effective_priority (t);
  if (t->effective_priority != new_effective_priority)
  {
    thread_set_effective_priority (t, new_priority);

    /* Check if priority is no longer the highest in the system */
    if (highest_thread_priority () > new_priority) 
      thread_yield ();
  }
  intr_set_level (old_level);
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->effective_priority;
}


/* Computes the maximum priority for a thread including all donated
   priorities. Does not change any thread state. Must be called with
   interrupts disabled. */
int
thread_get_effective_priority (struct thread *t)
{
  ASSERT (intr_get_level () == INTR_OFF);

  /* Initialize to our priority */
  int max_priority = t->priority;

  struct list_elem *e;
  struct lock *l;

  /* Iterate over all locks we're holding and find the max priority of
     each one's waiters */
  for (e = list_begin (&t->priority_holding); e != list_end
  (&t->priority_holding);
       e = list_next (e))
  {
    l = list_entry (e, struct lock, priority_holder);
    if (!list_empty (&l->semaphore.waiters))
    {
      /* Get the max priority from this lock's waiters */
      struct list_elem *max_item = 
        list_max (&l->semaphore.waiters, cmp_thread_priority, NULL);
      struct thread *waiter = 
        list_entry (max_item, struct thread, elem);

      if (waiter->effective_priority > max_priority)
        max_priority = waiter->effective_priority;
    }
  }

  return max_priority;
}

/* Sets the effective priority of this thread. Propagates to all threads
   that this thread waits on (recursively). Updates the ready queue as
   needed and waiting lists of all semaphores. Must be called with
   interrupts disabled.  TODO: update ordered lists to unordered and
   find_max to aovid this step. */
void
thread_set_effective_priority (struct thread *t, int new_priority)
{
  ASSERT (intr_get_level () == INTR_OFF);
  if (thread_mlfqs) return;

  /* Update the thread's priority */
  t->effective_priority = new_priority;

  /* Check if it is depending on other threads */
  if (t->priority_waiting != NULL)
  {
    struct thread *child = t->priority_waiting->holder;
    if (child->effective_priority < new_priority)
      thread_set_effective_priority (child, new_priority);
  }
}

static
struct thread *mlfqs_get_highest_priority_thread (void)
{
   int i;
   for (i = PRI_MAX; i >= PRI_MIN; i--) 
   {
     struct list *cur_list = &mlfqs_lists[i];
     if (!list_empty (cur_list)) 
     {
       return list_entry (list_front (cur_list), 
			  struct thread, mlfqs_elem);
     }
   }

   return NULL;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{
  struct thread *cur_thread = thread_current();
  cur_thread->nice = clamp (nice, NICE_MIN, NICE_MAX);
  enum intr_level old_level = intr_disable ();
  set_mlfqs_priority (cur_thread, compute_mlfqs_priority (cur_thread));

  /* if no longer highest priority, yield */
  struct thread *highest_priority = mlfqs_get_highest_priority_thread ();
  if (highest_priority && highest_priority->mlfqs_priority >
      cur_thread->mlfqs_priority)
  {
    thread_yield ();
  }
  intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return fp2int (fpmul (int2fp(100), load_avg));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  struct thread *t = thread_current ();

  return fp2int (fpmul (int2fp(100), t->recent_cpu));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  idle_thread->mlfqs_priority = PRI_MIN;
  idle_thread->effective_priority = PRI_MIN;
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->effective_priority = priority;
  list_init (&t->priority_holding);
  t->priority_waiting = NULL;
  t->nice = NICE_DEFAULT;
  t->recent_cpu = int2fp (0);
  t->mlfqs_priority = priority;
  t->magic = THREAD_MAGIC;
  list_push_back (&all_list, &t->allelem);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (!thread_mlfqs) 
  {
    if (!list_empty (&ready_list))
    {
      struct list_elem *max_item = 
        list_max (&ready_list, cmp_thread_priority, NULL);
      list_remove (max_item);
      return list_entry (max_item, struct thread, elem);
    }
  } else {
    struct thread *next = mlfqs_get_highest_priority_thread ();
    if (next)
    {
      mlfqs_num_ready--;
      list_remove (&next->mlfqs_elem);
      return next;
    }
  }
  return idle_thread;
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();

  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}


/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
