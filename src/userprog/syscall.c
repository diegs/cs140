#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static void syscall_handler (struct intr_frame *);

static inline void* frame_arg (struct intr_frame *f, int i) 
{
  return ((unsigned*)f->esp) + i;
}

static unsigned get_frame_syscall (struct intr_frame *f) 
{
  return *(unsigned*)frame_arg (f, 0);
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
  if (uaddr < PHYS_BASE)
    return get_user (uaddr);
  else
    return -1;
}

/* Writes BYTE to user address UDST. Returns true if successful, false
   if unsuccessful. */
static bool
put_byte (const uint8_t *udst, uint8_t byte)
{
  if (uaddr < PHYS_BASE)
    return put_user (udst, byte);
  else
    return false;
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void 
sys_write (struct intr_frame *f) 
{
  int fd = *(int*)frame_arg (f, 1);
  if (fd == 1) 
  {
    void* buffer = *(void**) frame_arg (f, 2);
    unsigned size = *(unsigned*) frame_arg (3);
    unsigned i = 0;
    for (i = 0; i < size; i++) 
    {
      //TODO: Complete implementation      
    }
  }
}

static void
syscall_handler (struct intr_frame *f) 
{
  unsigned syscall = get_frame_syscall (f);
  
  printf ("In syscall handlers:\n");

  switch (syscall) 
  {
    case SYS_HALT:
      printf ("Calling SYS_HALT, not implemented.\n");
      break;
    case SYS_EXIT:
      printf ("Calling SYS_EXIT, not implemented.\n");
      break;
    case SYS_EXEC:
      printf ("Calling SYS_EXEC, not implemented.\n");
      break;
    case SYS_WAIT:
      printf ("Calling SYS_WAIT, not implemented.\n");
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
      printf ("Calling SYS_WRITE, not implemented.\n");
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
}
