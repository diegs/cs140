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

/* Stores the information that allows us to identify which pages in
   virtual memory belong to a given mmapping. */
struct mmap_entry
{
  struct list_elem elem;    /* list_elem for storage in process_mmap */
  uint8_t *uaddr;           /* Virtual page that is backed by a file */
};

/* Stores the data that is common to all of the individual page 
   mappings for a single mmapped file. */
struct process_mmap
{
  struct list_elem elem;    /* list_elem for storage in a process */

  struct file *file;        /* The file that is backing our pages */
  struct list entries;      /* List of virtual pages in this mmap */  
  unsigned size;            /* Total size of the file */
  int id;                   /* mmap id for this mmap */
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
bool process_create_pcb (struct thread *t);

/* Functions for manipulating the mapping between fd and file* for
   a given process */
int process_add_file (struct thread *t, struct file *file, 
  const char* filename);
struct process_fd* process_get_file (struct thread *t, int fd);
void process_remove_file (struct thread *t, int fd);

/* Functions for manipulating mmapps for a given process */
struct process_mmap* 
mmap_create (const char *filename);
bool mmap_add (struct process_mmap *mmap, void* uaddr, 
                   unsigned offset);
void mmap_destroy (struct process_mmap *mmap);

int process_add_mmap (struct process_mmap *mmap);
bool process_remove_mmap (int id);
void process_mmap_file_close (struct file* file);

#endif /* userprog/process.h */
