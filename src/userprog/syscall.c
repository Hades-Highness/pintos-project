#include "devices/shutdown.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/synch.h"

static void syscall_handler(struct intr_frame*);
struct lock filesys_lock;

static void check_valid_ptr(const void *vaddr, size_t size) {
    for (size_t i = 0; i < size; i++) {
        const void *ptr = (const void *)((uint8_t *)vaddr + i);
        if (!is_user_vaddr(ptr)) {
             thread_current()->pcb->exit_status = -1;
             printf("%s: exit(-1)\n", thread_current()->pcb->process_name);
             process_exit();
             thread_exit();
        }
        if (pagedir_get_page(thread_current()->pcb->pagedir, ptr) == NULL) {
             thread_current()->pcb->exit_status = -1;
             printf("%s: exit(-1)\n", thread_current()->pcb->process_name);
             process_exit();
             thread_exit();
        }
    }
}

static void check_valid_string(const char *str) {
    check_valid_ptr((const void *)str, 1);
    while (true) {
        check_valid_ptr((const void *)str, 1);
        if (*str == '\0')
            break;
        str++;
    }
}

void syscall_init(void) { 
  lock_init(&filesys_lock);
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); 
}

static void syscall_handler(struct intr_frame* f) {
  check_valid_ptr((const void *)f->esp, sizeof(int));
  int syscall_number = *(int *)f->esp;

  switch (syscall_number) {
    case SYS_HALT: 
      shutdown_power_off(); 
      break;

    case SYS_EXIT:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(int));
      int status = *(int *)(f->esp + 4);
      thread_current()->pcb->exit_status = status;
      printf("%s: exit(%d)\n", thread_current()->pcb->process_name, status);
      process_exit();
      thread_exit();
      break;

    case SYS_PRACTICE:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(int));
      f->eax = *(int *)(f->esp + 4) + 1;
      break;

    case SYS_EXEC:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(char *));
      char *cmd = *(char **)(f->esp + 4);
      if (cmd == NULL || !is_user_vaddr(cmd) || pagedir_get_page(thread_current()->pcb->pagedir, cmd) == NULL) {
          f->eax = -1; 
          break;
      }
      check_valid_string(cmd);
      f->eax = process_execute(cmd);
      break;

    case SYS_FORK:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(char *));
      f->eax = process_fork((struct intr_frame *)f);
      break;

    case SYS_WAIT:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(int));
      f->eax = process_wait(*(int *)(f->esp + 4));
      break;

    case SYS_CREATE:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(char *));
      check_valid_ptr((const void *)(f->esp + 8), sizeof(unsigned));
      char *file_name = *(char **)(f->esp + 4);
      if (file_name == NULL) {
          thread_current()->pcb->exit_status = -1;
          printf("%s: exit(-1)\n", thread_current()->pcb->process_name);
          process_exit();
          thread_exit();
      }
      if (!is_user_vaddr(file_name)) { 
        f->eax = false; 
        break; 
      }
      check_valid_string(file_name);
      
      lock_acquire(&filesys_lock);
      f->eax = filesys_create(file_name, *(unsigned *)(f->esp + 8));
      lock_release(&filesys_lock);
      break;

    case SYS_REMOVE:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(char *));
      char *rem_name = *(char **)(f->esp + 4);
      if (rem_name == NULL) {
          thread_current()->pcb->exit_status = -1;
          printf("%s: exit(-1)\n", thread_current()->pcb->process_name);
          process_exit();
          thread_exit();
      }
      if (!is_user_vaddr(rem_name)) { 
        f->eax = false; 
        break; 
      }
      check_valid_string(rem_name);
      
      lock_acquire(&filesys_lock);
      f->eax = filesys_remove(rem_name);
      lock_release(&filesys_lock);
      break;

    case SYS_OPEN:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(char *));
      char *open_name = *(char **)(f->esp + 4);
      check_valid_string(open_name);
      
      lock_acquire(&filesys_lock);
      struct file *f_ptr = filesys_open(open_name);
      if (f_ptr == NULL) {
          f->eax = -1;
          lock_release(&filesys_lock);
      } else {
        struct thread *t = thread_current();
        int fd = t->pcb->next_fd;
        t->pcb->fd_table[fd] = f_ptr;
        f->eax = fd;
        while (fd < 128 && t->pcb->fd_table[fd] != NULL) fd++;
        t->pcb->next_fd = fd;
        lock_release(&filesys_lock);
      }
      break;

    case SYS_FILESIZE:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(int));
      int fd_size = *(int *)(f->esp + 4);
      if (fd_size >= 2 && fd_size < 128 && thread_current()->pcb->fd_table[fd_size] != NULL) {
        lock_acquire(&filesys_lock);
        f->eax = file_length(thread_current()->pcb->fd_table[fd_size]);
        lock_release(&filesys_lock);
      } else {
        f->eax = -1;
      }
      break;

    case SYS_READ: {
      check_valid_ptr((const void *)(f->esp + 4), 12);
      int fd = *(int *)(f->esp + 4);
      uint8_t *buffer = *(uint8_t **)(f->esp + 8);
      unsigned size = *(unsigned *)(f->esp + 12);
      check_valid_ptr(buffer, size);
      if (fd == 0) {
          for(unsigned i = 0; i < size; i++) 
              buffer[i] = input_getc();
          f->eax = size;
      }
      else if (fd >= 2 && fd < 128 && thread_current()->pcb->fd_table[fd] != NULL) {
          lock_acquire(&filesys_lock);
          struct file *file_ptr = thread_current()->pcb->fd_table[fd];
          int bytes_read = file_read(file_ptr, buffer, size);
          f->eax = bytes_read;
          
          lock_release(&filesys_lock);
      } else {
          f->eax = -1;
      }
      break;
    }

    case SYS_WRITE: {
      check_valid_ptr((const void *)(f->esp + 4), 12);
      int fd_w = *(int *)(f->esp + 4);
      void *buf_w = *(void **)(f->esp + 8);
      unsigned size_w = *(unsigned *)(f->esp + 12);
      
      check_valid_ptr(buf_w, size_w);

      if (fd_w == 1) { 
        putbuf(buf_w, size_w); 
        f->eax = size_w; 
      }
      else if (fd_w >= 2 && fd_w < 128 && thread_current()->pcb->fd_table[fd_w] != NULL) {
        lock_acquire(&filesys_lock);
        struct file *file_ptr = thread_current()->pcb->fd_table[fd_w];
        int bytes_written = file_write(file_ptr, buf_w, size_w);
        f->eax = bytes_written;
        lock_release(&filesys_lock);
      }
      else {
        f->eax = -1;
      }
      break;
    }

    case SYS_SEEK:
      check_valid_ptr((const void *)(f->esp + 4), 8);
      int fd_s = *(int *)(f->esp + 4);
      unsigned pos = *(unsigned *)(f->esp + 8);
      if (fd_s >= 2 && fd_s < 128 && thread_current()->pcb->fd_table[fd_s] != NULL) {
        lock_acquire(&filesys_lock);
        file_seek(thread_current()->pcb->fd_table[fd_s], pos);
        lock_release(&filesys_lock);
      }
      break;

    case SYS_TELL:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(int));
      int fd_t = *(int *)(f->esp + 4);
      if (fd_t >= 2 && fd_t < 128 && thread_current()->pcb->fd_table[fd_t] != NULL) {
        lock_acquire(&filesys_lock);
        f->eax = file_tell(thread_current()->pcb->fd_table[fd_t]);
        lock_release(&filesys_lock);
      } else {
        f->eax = -1;
      }
      break;

    case SYS_CLOSE:
      check_valid_ptr((const void *)(f->esp + 4), sizeof(int));
      int fd_c = *(int *)(f->esp + 4);
      if (fd_c >= 2 && fd_c < 128 && thread_current()->pcb->fd_table[fd_c] != NULL) {
        lock_acquire(&filesys_lock);
        file_ref_decrement_and_close(thread_current()->pcb->fd_table[fd_c]);
        thread_current()->pcb->fd_table[fd_c] = NULL;
        lock_release(&filesys_lock);
      }
      break;

    default:
      process_exit();
      thread_exit();
  }
}