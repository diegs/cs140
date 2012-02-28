#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
void syscall_close (int fd);
int syscall_open (const char *filename);

struct lock fd_all_lock;
#endif /* userprog/syscall.h */
