#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <list.h>
#include "threads/thread.h"
#include "threads/synch.h"
#include "filesys/file.h"

struct process_fd 
{
  struct list_elem elem;     /* List placement in owning process */
  struct file *file;
  const char* filename;
  int fd;
};

struct process_status
{
  tid_t tid;                 /* Child thread id */
  struct thread *t;          /* Child thread pointer */
  int status;                /* Exit status */
  struct condition cond;     /* Condition for signaling parent */
  struct lock l;             /* Lock for managing access to struct */
  struct list_elem elem;     /* List placement in parent */

  /* File system information */
  struct list fd_list;       /* List of file descriptors open in this
                                process */
  int next_fd;

  /* The file that spawned this process -- this must be kept open
     until the end of the execution of the thread */
  struct file* exec_file;
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


#endif /* userprog/process.h */
