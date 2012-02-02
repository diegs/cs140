#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static void syscall_handler (struct intr_frame *);

static inline void* frame_arg (struct intr_frame *f, int i) 
{
  return ((uint32_t*)f->esp) + i;
}

static uint32_t get_frame_syscall (struct intr_frame *f) 
{
  return *(uint32_t*)frame_arg (f, 0);
}

/* Reads a byte at user virtual address UADDR. UADDR must be below
   PHYS_BASE.  Returns the byte value if successful, -1 if a segfault
   occurred. */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}
 
/* Writes BYTE to user address UDST. UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte)
{
  int error_code;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

/* Reads a byte at user virtual address UADDR. Returns the byte value
   if successful, -1 if address was invalid. */
static int
get_byte (const uint8_t *uaddr)
{
  if (((void*)uaddr) < PHYS_BASE)
    return get_user (uaddr);
  else
    return -1;
}

/* Writes BYTE to user address UDST. Returns true if successful, false
   if unsuccessful. */
static bool
put_byte (uint8_t *udst, uint8_t byte)
{
  if ((void*)udst < PHYS_BASE)
    return put_user (udst, byte);
  else
    return false;
}

/* Like strlcpy in string.c but uses the safe kernel-user methods */
static size_t
user_strlcpy (char *dst, const char *src, size_t size)
{
  size_t i;
  int result;
  char byte;

  ASSERT (dst != NULL);
  ASSERT (src != NULL);

  if (size == 0) return 0;

  byte = 0;
  for (i = 0; i < size; i++)
  {
    /* Make sure memory access was valid */
    result = get_byte ((uint8_t*)src + i);
    if (result == -1) break;

    /* Read the byte */
    byte = *((char*)result);
    if (result == '\0') break;

    dst[i] = byte;
  }

  dst[i-1] = '\0';
  return i;
}

/**
 * Terminates Pintos by calling shutdown_power_off().
 *
 * Arguments:
 * - none
 * Returns: 
 * - none
 */
static void
sys_halt (struct intr_frame *f UNUSED)
{
  shutdown_power_off ();
}

/**
 * Terminates the current user program, returning status to the kernel.
 *
 * Arguments: 
 * - int status: status that is returned to the parent process
 * Returns: 
 * - none
 */
static void
sys_exit (struct intr_frame *f UNUSED)
{
  int status = *((int*)frame_arg (f, 1));
  struct process_status *ps = thread_current ()->p_status;
  if (ps != NULL)
  {
    /* Update status and notify any waiting parent of this */
    lock_acquire (&ps->l);
    ps->status = status;
    cond_signal (&ps->cond, &ps->l);
    lock_release (&ps->l);
  }
  process_exit ();
}

/**
 * Runs the executable whose name is given in cmd_line, passing any given
 * arguments, and returns the new process's program id (pid).
 *
 * Arguments: 
 * - const char *cmd_line: the command line to invoke.
 * Returns: 
 * - the new process' pid, or -1 if the program cannot load or run.
 */
static int
sys_exec (struct intr_frame *f)
{
  /* Copy commandline from user to kernel space */
  char *user_cmdline = *((char**)frame_arg (f, 1));
  char *kern_cmdline = palloc_get_page (0);
  if (kern_cmdline == NULL) return -1;
  user_strlcpy (kern_cmdline, user_cmdline, PGSIZE);

  /* Execute the process */
  tid_t tid = process_execute (kern_cmdline);

  /* Clean up and return results */
  palloc_free_page (kern_cmdline); 
  return (tid == TID_ERROR) ? -1 : tid;
}

/**
 * Waits for a child process pid and retrieves the child's exit status.
 *
 * Arguments: 
 * - int pid: the pid of the child process to wait on
 * Returns: 
 * - the exit status of the child process, or -1 if it not a valid child process
 */
static int
sys_wait (struct intr_frame *f UNUSED)
{
  int tid = *((int*)frame_arg (f, 1));
  return process_wait (tid);
}

static uint32_t
sys_write (struct intr_frame *f) 
{
  int fd = *(int*)frame_arg (f, 1);
  uint32_t result = 0;
  if (fd == 1) 
  {
    uint8_t* buffer = *(uint8_t**) frame_arg (f, 2);
    uint32_t size = *(uint32_t*) frame_arg (f, 3);
    uint32_t i = 0;
    for (i = 0; i < size; i++) 
    {
      int byte = get_byte (buffer + i);
      if (byte == -1) break;
      
      // TODO: Write characters to an intermediate buffer for
      // a single call to putbuf
      putbuf ((char*)&byte, 1);
      result++;
    }
  }

  // TODO: Handle other file descriptors

  return result; 
}


/* Registers the system call handler for internal interrupts. */
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Convenience bit-mashing method */
static uint32_t int_to_uint32_t (int i)
{
  return *((uint32_t*)i);
}

/* Handles system calls using the internal interrupt mechanism. The
   supported system calls are defined in lib/user/syscall.h */
static void
syscall_handler (struct intr_frame *f) 
{
  uint32_t syscall = get_frame_syscall (f);
  uint32_t eax = f->eax;

  switch (syscall) 
  {
    case SYS_HALT:
      sys_halt (f);
      break;
    case SYS_EXIT:
      sys_exit (f);
      break;
    case SYS_EXEC:
      eax = int_to_uint32_t (sys_exec (f));
      break;
    case SYS_WAIT:
      eax = int_to_uint32_t (sys_wait (f));
      break;
    case SYS_CREATE:
      printf ("Calling SYS_CREATE, not implemented.\n");
      break;
    case SYS_REMOVE:
      printf ("Calling SYS_REMOVE, not implemented.\n");
      break;
    case SYS_OPEN:
      printf ("Calling SYS_OPEN, not implemented.\n");
      break;
    case SYS_FILESIZE:
      printf ("Calling SYS_FILESIZE, not implemented.\n");
      break;
    case SYS_READ:
      printf ("Calling SYS_READ, not implemented.\n");
      break;
    case SYS_WRITE:
      eax = sys_write (f);
      break;
    case SYS_SEEK:
      printf ("Calling SYS_SEEK, not implemented.\n");
      break;
    case SYS_TELL:
      printf ("Calling SYS_TELL, not implemented.\n");
      break;
    case SYS_CLOSE:
      printf ("Calling SYS_CLOSE, not implemented.\n");
      break;
  }

  f->eax = eax;
}
