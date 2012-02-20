#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
void syscall_close (int fd);

struct lock fd_all_lock;
#endif /* userprog/syscall.h */
