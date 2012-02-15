#include <stdint.h>
#include <stdio.h>
#include <syscall-nr.h>
#include <hash.h>
#include <string.h>
#include <stdlib.h>
#include "threads/malloc.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

struct fd_hash
{
  char *filename;
  int count;
  bool delete;
  struct hash_elem elem;
};

struct hash fd_all;
struct lock fd_all_lock;

static struct fd_hash *
fd_hash_init () 
{
  struct fd_hash *fd = calloc (1, sizeof (struct fd_hash));
  return fd;
}

static void fd_hash_destroy (struct fd_hash *h)
{
  hash_delete (&fd_all, &h->elem);
  if (h->filename != NULL) free (h->filename);
  free (h);
}


static unsigned hash_hash_fd_hash (const struct hash_elem *e, 
                            void *aux UNUSED) 
{
  struct fd_hash *e_fd = hash_entry (e, struct fd_hash, elem);
  unsigned hash_val = hash_string (e_fd->filename);
  return hash_val;
}

static bool hash_less_fd_hash (const struct hash_elem *a,
                             const struct hash_elem *b,
                             void *aux UNUSED)
{
  struct fd_hash *a_fd = hash_entry (a, struct fd_hash, elem);
  struct fd_hash *b_fd = hash_entry (b, struct fd_hash, elem);

  return strcmp (a_fd->filename, b_fd->filename) < 0;
}

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
  /*Get the first page this data spans */
  uint32_t start_page = (uint32_t)ptr / PGSIZE;

  /*Get the last page this data spans */
  uint32_t end_page = (uint32_t)(ptr + size) / PGSIZE;
	
  /* Check the first byte of each page */
  uint32_t page;
  for (page = start_page; page <= end_page; page++)
  {
	unsigned char * page_first_byte = (unsigned char *)(page * PGSIZE);
	if (get_byte (page_first_byte) == -1)
	{
	  process_kill ();
	}
  }
  return;
}

/* Verifies that an entire string is vald memory */
static void
memory_verify_string (const char *str)
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

/* Convenience method for dereferencing a frame argument */
static inline void* frame_arg (const struct intr_frame *f, const int i) 
{
  return ((uint32_t*)f->esp) + i;
}

/* Convenience method for getting an int out of a frame argument safely */
static inline int 
frame_arg_int (const struct intr_frame *f, const int i)
{
  void *arg = frame_arg (f, i);
  memory_verify (arg, sizeof (int));
  return *((int*)arg);
}

/* Convenience method for getting a pointer out of a frame argument safely */
static inline void *
frame_arg_ptr (const struct intr_frame *f, const int i)
{
  void *arg = frame_arg (f, i);
  memory_verify (arg, sizeof (void**));
  return *((void**)arg);
}

/* Convenience method for accessing the syscall safely */
static uint32_t
get_frame_syscall (const struct intr_frame *f) 
{
  return frame_arg_int (f, 0);
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
sys_halt (const struct intr_frame *f UNUSED)
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
sys_exit (const struct intr_frame *f)
{
  thread_current ()->exit_code = frame_arg_int (f, 1);
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
sys_exec (const struct intr_frame *f)
{
  /* Check the argument */
  char *cmdline = frame_arg_ptr (f, 1);
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
sys_wait (const struct intr_frame *f UNUSED)
{
  return process_wait (frame_arg_int (f, 1));
}

/* Check to see if filename is our hash table */
static struct fd_hash* get_fd_hash (const char* filename)
{
  struct fd_hash fd_sample;
  fd_sample.filename = (char*) filename;

  struct hash_elem *elem = hash_find (&fd_all, &fd_sample.elem);
  struct fd_hash *fd_found = NULL;

  if (elem != NULL)
    fd_found = hash_entry (elem, struct fd_hash, elem);

  return fd_found;
}


static bool
sys_create (const struct intr_frame *f)
{
  const char *filename = frame_arg_ptr (f, 1);
  uint32_t initial_size = frame_arg_int (f, 2);

  memory_verify_string (filename);

  lock_acquire (&fd_all_lock);
  bool ret = filesys_create (filename, initial_size);
  lock_release (&fd_all_lock);

  return ret;
}

static bool 
sys_remove (const struct intr_frame *f) 
{
  const char *filename = frame_arg_ptr (f, 1);
  memory_verify_string (filename);

  lock_acquire (&fd_all_lock);
  struct fd_hash *fd_found = get_fd_hash (filename); 

  bool result = false;
  /* Only entries with count > 0 are stored */
  if (fd_found) 
  {
    fd_found->delete = true;
    result = true;
  } else {
    result = filesys_remove (filename);
  }
  lock_release (&fd_all_lock);
  return result;
}


static int
sys_open (const struct intr_frame *f) 
{
  const char *filename = frame_arg_ptr (f, 1);
  memory_verify_string (filename);

  lock_acquire (&fd_all_lock);
  struct file* file = filesys_open (filename); 
  if (file == NULL) 
  {
    lock_release (&fd_all_lock);
    return -1;
  }

  struct fd_hash *fd_found = get_fd_hash (filename); 

  /* Update count */
  if (fd_found == NULL) 
  {
    fd_found = fd_hash_init ();
	if (fd_found == NULL)
	{
	  lock_release(&fd_all_lock);
	  return -1;
    }
	fd_found->filename = strdup (filename);
    hash_insert (&fd_all, &fd_found->elem);
  }
  /* Makes sure it isn't marked for deletion */
  if (fd_found->delete)
  {
    lock_release (&fd_all_lock);
    return -1;
  }
	
  fd_found->count++;
  int fd = process_add_file (thread_current (), 
                              file, fd_found->filename);
  lock_release (&fd_all_lock);

  return fd;
}

static int32_t
sys_filesize (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);

  struct process_fd *pfd = process_get_file (thread_current (), fd);
  if (pfd == NULL) return -1;

  lock_acquire (&fd_all_lock);
  int len = file_length (pfd->file);
  lock_release (&fd_all_lock);
  return len;
}


static void
sys_seek (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);
  off_t pos = *(off_t*)frame_arg (f, 2);

  struct process_fd *pfd = process_get_file (thread_current (), fd);
  if (pfd == NULL) return;
 
  lock_acquire (&fd_all_lock);
  file_seek (pfd->file, pos);
  lock_release (&fd_all_lock);
}

static uint32_t
sys_tell (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);

  struct process_fd *pfd = process_get_file (thread_current (), fd);
  if (pfd == NULL) return -1;
 
  lock_acquire (&fd_all_lock);
  uint32_t tell = file_tell (pfd->file);
  lock_release (&fd_all_lock);

  return tell;
}

void
syscall_close (int fd)
{
  lock_acquire (&fd_all_lock);
  struct process_fd *pfd = process_get_file (thread_current (), fd);
  if (pfd == NULL) {
    lock_release (&fd_all_lock);
    return;
  }
  struct fd_hash *fd_found = get_fd_hash (pfd->filename); 
  if (fd_found == NULL)
  {
	lock_release(&fd_all_lock);
	return;
  }
  file_close (pfd->file);

  /* Perform syscall level bookkeeping */
  fd_found->count--;
  if (fd_found->count == 0) 
  {
    if (fd_found->delete) filesys_remove (pfd->filename);
    fd_hash_destroy(fd_found);
  }

  /* Remove the file from the process */
  process_remove_file (thread_current (), fd);

  lock_release (&fd_all_lock);

}

static void
sys_close (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);
  syscall_close (fd);
}


static int32_t 
sys_read (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);
  char *user_buffer = frame_arg_ptr (f, 2);
  size_t user_size = frame_arg_int (f, 3);

  memory_verify (user_buffer, user_size);

  int result = -1;

  /* Special case for reading from the keyboard */
  if (fd == 0)
  {
    size_t read_size = 0;
    while (read_size < user_size) {
      user_buffer[read_size] = input_getc ();
      read_size++;
    }

    result = read_size;
  } else {

    struct process_fd *pfd = process_get_file (thread_current (), fd);
    if (pfd == NULL) return -1;

    lock_acquire (&fd_all_lock);
    result = file_read (pfd->file, user_buffer, user_size);
    lock_release (&fd_all_lock);
  }

  return result;
}

static int
sys_write (const struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);
  const char* buffer = frame_arg_ptr (f, 2);
  memory_verify_string (buffer);
  size_t size = frame_arg_int (f, 3);

  // Handle special case for writing to the console
  if (fd == 1) 
  {
    putbuf (buffer, size);
    return size;
  } 

  // Handle rest of file descriptors
  struct process_fd *pfd = process_get_file (thread_current (), fd);
  if (pfd == NULL) return 0;

  lock_acquire (&fd_all_lock);
  int result =  file_write(pfd->file, buffer, size);
  lock_release (&fd_all_lock);

  return result;
}

/* Registers the system call handler for internal interrupts. */
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  
  hash_init (&fd_all, hash_hash_fd_hash, hash_less_fd_hash, NULL);
  lock_init (&fd_all_lock);
}

/* Handles system calls using the internal interrupt mechanism. The
   supported system calls are defined in lib/user/syscall.h */
static void
syscall_handler (struct intr_frame *f) 
{
  /* Integrity-check the return pointer */
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
      eax = sys_open (f);
      break;
    case SYS_FILESIZE:
      eax = sys_filesize (f);
      break;
    case SYS_READ:
      eax = sys_read (f);
      break;
    case SYS_WRITE:
      eax = sys_write (f);
      break;
    case SYS_SEEK:
      sys_seek (f);
      break;
    case SYS_TELL:
      sys_tell (f);
      break;
    case SYS_CLOSE:
      sys_close (f);
      break;
  }

  /* Set return value */
  f->eax = eax;
}
