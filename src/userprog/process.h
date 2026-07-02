#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "lib/kernel/list.h"
#include <stdint.h>

struct intr_frame;

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);

/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct child_process {
  int pid;                        /* The child's PID, which matches its TID. */
  bool load_success;              /* Whether exec completed successfully. */
  struct semaphore load_sema;     /* Synchronization semaphore for exec. */
  struct semaphore exit_sema;     /* Synchronization semaphore for wait. */
  int exit_status;                /* Exit status reported by the child. */
  bool exited;                    /* Whether the child has already exited. */
  bool waited;                    /* Whether the parent has already waited. */
  struct list_elem elem;          /* Entry in the parent's child list. */
};

struct process {
  /* Owned by process.c. */
  uint32_t* pagedir;          /* Page directory. */
  char process_name[16];      /* Name of the main thread */
  struct thread* main_thread; /* Pointer to main thread */
  int exit_status;
  struct semaphore load_sema;
  struct list children;
  struct file *fd_table[128];
  struct file* executable;
  bool load_success;
  int next_fd;
};

void userprog_init(void);

pid_t process_execute(const char* file_name);
pid_t process_fork(struct intr_frame*);
int process_wait(pid_t);
void process_exit(void);
void process_activate(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

#endif /* userprog/process.h */
