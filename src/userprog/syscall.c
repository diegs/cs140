#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include <stdint.h>

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

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
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

static void
syscall_handler (struct intr_frame *f) 
{
  uint32_t syscall = get_frame_syscall (f);

  uint32_t eax = f->eax;

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
