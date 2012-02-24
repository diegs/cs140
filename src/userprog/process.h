#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <list.h>
#include "threads/thread.h"
#include "threads/synch.h"
#include "filesys/file.h"

#define INVALID_MMAP_ID -1

struct process_fd 
{
  struct list_elem elem;     /* List placement in owning process */
  struct file *file;         /* Handle to the file */
  const char* filename;      /* This memory does not need to be freed
                                because it is handled by the global
                                list of file descriptors in syscall.c*/
  int fd;
};

struct mmap_entry
{
  struct list_elem elem;
  struct s_page_entry *spe;
};

struct process_mmap
{
  struct list_elem elem;

  struct file *file;
  struct list entries;
  unsigned size;
  int id;
};

struct process_status
{
  tid_t tid;                 /* Child thread id */
  struct thread *t;          /* Child thread pointer */
  int status;                /* Exit status */
  struct condition cond;     /* Condition for signaling parent */
  struct lock l;             /* Lock for managing access to struct */
  struct list_elem elem;     /* List placement in parent */

};

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
 
void process_create_pcb (struct thread *t);

/* Functions for manipulating the mapping between fd and file* for
   a given process */
int process_add_file (struct thread *t, struct file *file, 
  const char* filename);
struct process_fd* process_get_file (struct thread *t, int fd);
void process_remove_file (struct thread *t, int fd);

/* Functions for manipulating mmapps for a given process */
struct process_mmap* 
mmap_create (struct file *file);
bool mmap_add (struct process_mmap *mmap, void* uaddr, 
                   unsigned offset);
void mmap_destroy (struct process_mmap *mmap);

int process_add_mmap (struct process_mmap *mmap);

#endif /* userprog/process.h */
