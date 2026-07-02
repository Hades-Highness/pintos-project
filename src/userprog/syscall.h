#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

/* System call entry points and shared filesystem lock. */
void syscall_init(void);
extern struct lock filesys_lock;

#endif /* userprog/syscall.h */
