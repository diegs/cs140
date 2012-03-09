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

static struct fd_hash*
fd_hash_init (void)
{
  struct fd_hash *fd = calloc (1, sizeof (struct fd_hash));
  return fd;
}

static void
fd_hash_destroy (struct fd_hash *h)
{
  hash_delete (&fd_all, &h->elem);
  if (h->filename != NULL) free (h->filename);
  free (h);
}

static unsigned
hash_hash_fd_hash (const struct hash_elem *e, void *aux UNUSED) 
{
  struct fd_hash *e_fd = hash_entry (e, struct fd_hash, elem);
  unsigned hash_val = hash_string (e_fd->filename);
  return hash_val;
}

static bool
hash_less_fd_hash (const struct hash_elem *a, const struct hash_elem *b,
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
  if (size == 0) return;
  /*Get the first page this data spans */
  uint32_t start_page = (uint32_t)ptr / PGSIZE;

  /*Get the last page this data spans */
  uint32_t end_page = (uint32_t)(ptr + size - 1) / PGSIZE;
	
  /* Check the first byte of each page */
  uint32_t page;
  for (page = start_page; page <= end_page; page++)
  {
    uint8_t *page_first_byte = (uint8_t *)(page * PGSIZE);
    if (get_byte (page_first_byte) == -1)
    {
      process_kill ();
    }
  }
  return;
}

/* Verifies that size memory at ptr is valid */
static void
memory_verify_write (void *ptr, size_t size)
{
  if (size == 0) return;
  /*Get the first page this data spans */
  uint32_t start_page = (uint32_t)ptr / PGSIZE;

  /*Get the last page this data spans */
  uint32_t end_page = (uint32_t)(ptr + size - 1) / PGSIZE;
	
  /* Check the first byte of each page */
  uint32_t page;
  for (page = start_page; page <= end_page; page++)
  {
    uint8_t *page_first_byte = (uint8_t *)(page * PGSIZE);
    if ((void *)page_first_byte < ptr) page_first_byte = ptr;
    if (put_byte (page_first_byte, '\0') == -1)
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
 * Returns a copy of path, converted into an absolute path if necessary.
 */
static char *
path_make_absolute (const char *path)
{
  char *abs, *cwd;

  /* Already an absolute path */
  if (path[0] == '/')
    return strdup (path);
    
  /* Prepend current working directory */
  cwd = filesys_path (thread_get_cwd ());
  if (cwd == NULL) return NULL;

  abs = malloc (strlen (cwd) + strlen (path) + 1);
  if (abs == NULL) return NULL;

  strncpy (abs, cwd, strlen (cwd));
  strncpy (abs + strlen (cwd), path, strlen (path));

  return abs;
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

  bool ret = filesys_create (filename, initial_size);

  return ret;
}

static bool 
sys_remove (const struct intr_frame *f) 
{
  const char *filename = frame_arg_ptr (f, 1);
  memory_verify_string (filename);

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
  return result;
}

int
syscall_open (const char *filename) 
{
  struct file* file = filesys_open (filename); 
  if (file == NULL) 
  {
    return -1;
  }

  struct fd_hash *fd_found = get_fd_hash (filename); 

  /* Update count */
  if (fd_found == NULL) 
  {
    fd_found = fd_hash_init ();
    if (fd_found == NULL)
    {
      return -1;
    }
    fd_found->filename = strdup (filename);
    hash_insert (&fd_all, &fd_found->elem);
  }
  /* Makes sure it isn't marked for deletion */
  if (fd_found->delete)
  {
    return -1;
  }

  fd_found->count++;
  int fd = process_add_file (thread_current (), 
                             file, fd_found->filename);

  return fd;
}

static int
sys_open (const struct intr_frame *f) 
{
  const char *filename = frame_arg_ptr (f, 1);
  memory_verify_string (filename);

  return syscall_open (filename);
}

static int32_t
sys_filesize (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);

  struct process_fd *pfd = process_get_file (thread_current (), fd);
  if (pfd == NULL) return -1;

  int len = file_length (pfd->file);
  return len;
}

static void
sys_seek (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);
  off_t pos = *(off_t*)frame_arg (f, 2);

  struct process_fd *pfd = process_get_file (thread_current (), fd);
  if (pfd == NULL) return;
 
  file_seek (pfd->file, pos);
}

static uint32_t
sys_tell (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);

  struct process_fd *pfd = process_get_file (thread_current (), fd);
  if (pfd == NULL) return -1;
 
  uint32_t tell = file_tell (pfd->file);
  return tell;
}

void
syscall_close (int fd)
{
  struct process_fd *pfd = process_get_file (thread_current (), fd);
  if (pfd == NULL) {
    return;
  }
  struct fd_hash *fd_found = get_fd_hash (pfd->filename); 
  if (fd_found == NULL)
  {
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
}

static void
sys_close (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);
  syscall_close (fd);
}

/**
 * Changes the current working directory of the process to dir, which may be
 * relative or absolute. Returns true if successful, false on failure.
 */
static bool
sys_chdir (struct intr_frame *f)
{
  const char *dir = frame_arg_ptr (f, 1);
  memory_verify_string (dir);

  /* TODO implement */
  return false;
}

/**
 * Creates the directory named dir, which may be relative or absolute. Returns
 * true if successful, false on failure. Fails if dir already exists or if any
 * directory name in dir, besides the last, does not already exist. That is,
 * mkdir("/a/b/c") succeeds only if /a/b already exists and /a/b/c does not.
 */
static bool
sys_mkdir (struct intr_frame *f)
{
  const char *dir = frame_arg_ptr (f, 1);
  memory_verify_string (dir);

  return filesys_mkdir (path_make_absolute (dir));
}

/**
 * Reads a directory entry from file descriptor fd, which must represent a
 * directory. If successful, stores the null-terminated file name in name,
 * which must have room for READDIR_MAX_LEN + 1 bytes, and returns true. If no
 * entries are left in the directory, returns false.
 */
static bool
sys_readdir (struct intr_frame *f)
{
  int fd = frame_arg_int (f, 1);
  const char *name = frame_arg_ptr (f, 2);
  memory_verify_string (name);

  /* TODO implement */
  return false;
}

/**
 * Returns true if fd represents a directory, false if it represents an
 * ordinary file.
 */ 
static bool
sys_isdir (struct intr_frame *f)
{
  int fd = frame_arg_int (f, 1);

  /* TODO implement */
  return false;
}

/**
 * Returns the inode number of the inode associated with fd, which may
 * represent an ordinary file or a directory.
 */
static int
sys_inumber (struct intr_frame *f)
{
  int fd = frame_arg_int (f, 1);

  /* TODO implement */
  return -1;
}

/* This function performs some file operation one page at a time so
   that we do not need to worry about having a frame removed from
   under us */
static int
safe_file_block_ops (struct file *file, char *buffer, size_t size, bool write)
{
  size_t size_accum = 0;
  bool read = !write;

  char * tmp_buf = malloc(PGSIZE);
  if (tmp_buf == NULL) return -1;

  while (size_accum < size) 
  {
    int cur_size = size - size_accum;
    if (cur_size > PGSIZE) cur_size = PGSIZE;

    char *cur_buff = buffer + size_accum;

    if (write)
      memcpy(tmp_buf, cur_buff, cur_size);
    int op_result;
    if (read)
      op_result = file_read (file, tmp_buf, cur_size);
    else
      op_result = file_write (file, tmp_buf, cur_size);

    if (read)
      memcpy(cur_buff, tmp_buf, cur_size);

    size_accum += op_result;
    
    if (op_result != cur_size) break;
  }
  free(tmp_buf);
  return size_accum;
}

static int32_t 
sys_read (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);
  char *user_buffer = frame_arg_ptr (f, 2);
  size_t user_size = frame_arg_int (f, 3);

  memory_verify(user_buffer, user_size);
  memory_verify_write (user_buffer, user_size);

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

    result = safe_file_block_ops (pfd->file, user_buffer, user_size, false);
  }

  return result;
}

static int
sys_write (const struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);
  const char* buffer = frame_arg_ptr (f, 2);
  size_t size = frame_arg_int (f, 3);
  memory_verify ((void *)buffer, size);

  // Handle special case for writing to the console
  if (fd == 1) 
  {
    putbuf (buffer, size);
    return size;
  } 

  // Handle rest of file descriptors
  struct process_fd *pfd = process_get_file (thread_current (), fd);
  if (pfd == NULL) return 0;

  int result = safe_file_block_ops 
    (pfd->file, (void*)buffer, size, true);

  return result;
}

static int
sys_mmap (struct intr_frame *f) 
{
  int fd = frame_arg_int (f, 1);
  void *uaddr = frame_arg_ptr (f, 2);

  /* Verify that the uaddr is page aligned and is not NULL*/
  if (pg_round_down (uaddr) != uaddr) return -1;
  if (uaddr == NULL) return -1;

  /* Grab file associated with fd */
  struct thread *t = thread_current ();
  struct process_fd *pfd = process_get_file (t, fd);
  if (pfd == NULL) return -1;

  struct process_mmap *mmap = mmap_create (pfd->filename);
  if (mmap == NULL) return -1;

  /* Break file into pages, making sure to note the number of zeros
     in the remainder of the last page, and making sure that none of
     the addresses we want to add overlap with the stack, the data
     segment or the code segment (i.e. there is no existing virtual
     mapping in the page directory). All of this 
     checking/miscellaneous stuff is handled by mmap_add*/
  uint32_t offset = 0;
  while (offset < mmap->size)
  {
    bool success = mmap_add (mmap, uaddr + offset, offset);
    if (!success) 
    {
      mmap_destroy (mmap);
      return -1;
    }

    offset += PGSIZE;
  }

  /* Generate a valid mapid_t for the process */
  return process_add_mmap (mmap);
}

static void 
sys_munmap (struct intr_frame *f UNUSED)
{
  int id = frame_arg_int (f, 1);

  process_remove_mmap (id);
}

/* Registers the system call handler for internal interrupts. */
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  
  hash_init (&fd_all, hash_hash_fd_hash, hash_less_fd_hash, NULL);
}

/* Handles system calls using the internal interrupt mechanism. The
   supported system calls are defined in lib/user/syscall.h */
static void
syscall_handler (struct intr_frame *f) 
{
  /* Integrity-check the return pointer */
  memory_verify ((void*)f->esp, sizeof (void*));
#ifdef VM
  thread_current ()->saved_esp = f->esp;
  thread_current ()->syscall_context = true;
#endif
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
    eax = sys_tell (f);
    break;
  case SYS_CLOSE:
    sys_close (f);
    break;
  case SYS_CHDIR:
    eax = sys_chdir (f);
    break;
  case SYS_MKDIR:
    eax = sys_mkdir (f);
    break;
  case SYS_READDIR:
    eax = sys_readdir (f);
    break;
  case SYS_ISDIR:
    eax = sys_isdir (f);
    break;
  case SYS_INUMBER:
    eax = sys_inumber (f);
    break;
  case SYS_MMAP:
    eax = sys_mmap (f);
    break;
  case SYS_MUNMAP:
    sys_munmap (f);
    break;
  }
  thread_current ()->syscall_context = false;
  /* Set return value */
  f->eax = eax;
}
