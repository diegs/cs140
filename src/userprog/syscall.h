#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#define READDIR_MAX_LEN 14

void syscall_init (void);
void syscall_close (int fd);
int syscall_open (const char *filename);

#endif /* userprog/syscall.h */
