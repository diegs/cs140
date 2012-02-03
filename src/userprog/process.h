#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <list.h>
#include "threads/thread.h"
#include "threads/synch.h"

#define PROCESS_RUNNING -2

struct process_status
{
  tid_t tid;                 /* Child thread id */
  int status;                /* Exit status */
  bool parent_alive;         /* Flag for parent waiting */
  struct condition cond;     /* Condition for signaling parent */
  struct lock l;             /* Lock for managing access to struct */
  struct list_elem elem;     /* List placement in parent */
};

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
