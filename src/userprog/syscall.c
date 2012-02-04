#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#define SYSWRITE_BSIZE 256

static void syscall_handler (struct intr_frame *);

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

/* Kills a process with error code -1 */
static void
process_kill (void)
{
  thread_current ()->exit_code = -1;
  thread_exit ();
}

/* Verifies that size memory at ptr is valid */
static void
memory_verify (void *ptr, size_t size)
{
  size_t i;
  for (i=0; i<size; i++)
  {
    if (get_byte (((unsigned char*)ptr) + i) == -1)
      process_kill ();
  }
}

/* Verifies that an entire string is vald memory */
static void
memory_verify_string (char *str)
{
  while (true)
  {
    int result = get_byte ((unsigned char*)str);
    if (result == -1)
      process_kill ();
    else if ((char)result == '\0')
      return;
    str++;
  }
}

/* Like memcpy, but copies from userland. Returns number of bytes copied,
   or -1 if error occurred */
static int
user_memcpy (void *dst, const void *src, size_t size)
{
  size_t i;
  int result;
  char byte;

  ASSERT (dst != NULL);
  ASSERT (src != NULL);

  byte = 0;
  for (i = 0; i < size; i++)
  {
    /* Make sure memory access was valid */
    result = get_byte ((uint8_t*)src + i);
    if (result == -1) return -1;

    /* Read the byte */
    byte = (char)result;
    ((char*)dst)[i] = byte;
  }

  return i;
}

/* Convenience method for dereferencing a frame argument */
static inline void* frame_arg (struct intr_frame *f, int i) 
{
  return ((uint32_t*)f->esp) + i;
}

/* Convenience method for accessing the syscall safely */
static uint32_t get_frame_syscall (struct intr_frame *f) 
{
  void *arg0 = frame_arg (f, 0);
  memory_verify (arg0, sizeof (uint32_t));
  return *((uint32_t*)arg0);
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
sys_exit (struct intr_frame *f)
{
  void *arg1 = frame_arg (f, 1);
  memory_verify (arg1, sizeof (int));
  thread_current ()->exit_code = *((int*)arg1);
  thread_exit ();
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
  /* Check the argument */
  void *arg1 = frame_arg (f, 1);
  memory_verify (arg1, sizeof (char**));
  char *cmdline = *((char**)arg1);
  memory_verify_string (cmdline);

  /* Execute the process */
  tid_t tid = process_execute (cmdline);
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
  void *arg1 = frame_arg (f, 1);
  memory_verify (arg1, sizeof (int));
  return process_wait (*((int*)arg1));
}

static bool
sys_create (struct intr_frame *f)
{
  const char *filename = *(char**)frame_arg (f, 1);
  uint32_t initial_size = *(uint32_t*)frame_arg (f, 2);

  return filesys_create (filename, initial_size);
}

static bool 
sys_remove (struct intr_frame *f) 
{
  const char *filename = *(char**)frame_arg(f, 1);
  return filesys_remove (filename);
}

static uint32_t 
sys_open (struct intr_frame *f) 
{
  const char *filename = *(char**)frame_arg(f, 1);

  // TODO: implement this correctly

  return 0;
}


static uint32_t
sys_write (struct intr_frame *f) 
{
  int fd = *(int*)frame_arg (f, 1);
  uint32_t result = 0;
  if (fd == 1) 
  {
    char* user_buffer = *(char**) frame_arg (f, 2);
    size_t size_total = *(size_t*) frame_arg (f, 3);
    size_t size_remain = size_total;
    
    char kernel_buffer[SYSWRITE_BSIZE];
    while (size_remain > 0)
    {
      size_t bytes_attempt = 
        SYSWRITE_BSIZE > size_remain ? size_remain : SYSWRITE_BSIZE;

      size_t bytes_copied = 
        user_memcpy (kernel_buffer, user_buffer, bytes_attempt);

      putbuf (kernel_buffer, bytes_copied);

      result += bytes_copied;
      if (bytes_copied < bytes_attempt) break;

      size_remain -= bytes_copied;
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

/* Handles system calls using the internal interrupt mechanism. The
   supported system calls are defined in lib/user/syscall.h */
static void
syscall_handler (struct intr_frame *f) 
{
  /* Sanity check the return pointer */
  memory_verify ((void*)f->esp, sizeof (void*));

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
      eax = sys_exec (f);
      break;
    case SYS_WAIT:
      eax = sys_wait (f);
      break;
    case SYS_CREATE:
      eax = sys_create (f);
      break;
    case SYS_REMOVE:
      eax = sys_remove (f);
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

  /* Set return value */
  f->eax = eax;
}
