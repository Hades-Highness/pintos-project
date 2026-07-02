#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* file_name, void (**eip)(void), void** esp);
bool setup_thread(void (**eip)(void), void** esp);

/* Structure temporaire pour passer les arguments au thread fils */
struct exec_args {
  char* file_name;                  /* La ligne de commande */
  struct child_process* child;      /* La structure de synchro */
};

void userprog_init(void) {
  struct thread* t = thread_current();

  t->pcb = calloc(1, sizeof(struct process));
  ASSERT(t->pcb != NULL);

  t->pcb->pagedir = NULL;
  t->pcb->main_thread = t;
  t->pcb->exit_status = 0;
  t->pcb->load_success = true;
  strlcpy(t->pcb->process_name, t->name, sizeof t->pcb->process_name);
  list_init(&t->pcb->children);
  sema_init(&t->pcb->load_sema, 0);
}

/* Process execution setup and startup. */
pid_t process_execute(const char* file_name) {
  char* fn_copy;
  tid_t tid;

  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy(fn_copy, file_name, PGSIZE);

  char *save_ptr;
  char *file_name_copy = palloc_get_page(0);
  if (file_name_copy == NULL) {
    palloc_free_page(fn_copy);
    return TID_ERROR;
  }
  strlcpy(file_name_copy, file_name, PGSIZE);
  
  char *real_name = strtok_r(file_name_copy, " ", &save_ptr);
  
  /* Allocate and initialize the child tracking structure. */
  struct child_process *child = malloc(sizeof(struct child_process));
  if (child == NULL) {
    palloc_free_page(file_name_copy);
    palloc_free_page(fn_copy);
    return TID_ERROR;
  }
  sema_init(&child->load_sema, 0);
  sema_init(&child->exit_sema, 0);
  child->load_success = false;
  child->exit_status = -1;
  child->exited = false;
  child->waited = false;

  /* Package the exec arguments for the child start routine. */
  struct exec_args *args = malloc(sizeof(struct exec_args));
  if (args == NULL) {
    free(child);
    palloc_free_page(file_name_copy);
    palloc_free_page(fn_copy);
    return TID_ERROR;
  }
  args->file_name = fn_copy;
  args->child = child;

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create(real_name, PRI_DEFAULT, start_process, args);
  
  palloc_free_page(file_name_copy); 

  if (tid == TID_ERROR) {
    palloc_free_page(fn_copy);
    free(args);
    free(child);
    return TID_ERROR;
  }

  /* Register the child PID and add it to the parent's child list when a valid PCB exists. */
  child->pid = (int)tid;
  struct process *current_pcb = thread_current()->pcb;
  if (current_pcb != NULL) {
    list_push_back(&current_pcb->children, &child->elem);
  }

  /* Wait for the child to report whether exec succeeded. */
  sema_down(&child->load_sema);
  
  bool success = child->load_success;

  /* If the parent has no PCB, the kernel thread must free the child structure because no child list owns it. */
  if (!success) {
    /* If exec failed, the child thread will have already signaled load failure.
       The child_process structure remains owned by the parent until wait(). */
    return TID_ERROR;
  }

  return tid;
}

/* Structure used to pass fork initialization data to the child thread. */
struct fork_args {
  struct thread *parent;
  struct intr_frame parent_if;
  struct child_process *child_frame;
};

/* External declaration of the global filesystem lock. */
extern struct lock filesys_lock;

void file_ref_increment(struct file *f);
void file_ref_decrement_and_close(struct file *f);

/* Entry point for the child thread created by process_fork. */
static void fork_thread_entry(void *args_) {
  struct fork_args *args = (struct fork_args *)args_;
  struct thread *cur = thread_current();
  struct thread *parent = args->parent;
  struct intr_frame local_if;
  bool success = true;

  /* Allocate the child's PCB. */
  struct process *new_pcb = malloc(sizeof(struct process));
  if (new_pcb == NULL) {
    success = false;
  } else {
    cur->pcb = new_pcb;
    new_pcb->main_thread = cur;
    strlcpy(new_pcb->process_name, cur->name, sizeof(new_pcb->process_name));
    list_init(&new_pcb->children);
    new_pcb->executable = NULL;
    new_pcb->exit_status = 0;
    new_pcb->next_fd = 2;
    for (int i = 0; i < 128; i++) {
      new_pcb->fd_table[i] = NULL;
    }
    
    /* Create a new page directory for the child process. */
    new_pcb->pagedir = pagedir_create();
    if (new_pcb->pagedir == NULL) {
      success = false;
    } else {
      /* Activer le pagedir pour pouvoir copier les pages utilisateur dedans */
      process_activate();

      /* 3. Copier l'espace d'adressage virtuel du parent */
      uint32_t *pd_parent = parent->pcb->pagedir;
      uint32_t *vaddr;
      
      /* Copy the parent's user address space page by page. */
      for (vaddr = 0; vaddr < (uint32_t *)PHYS_BASE; vaddr += PGSIZE / sizeof(uint32_t)) {
        void *kpage_parent = pagedir_get_page(pd_parent, vaddr);
        if (kpage_parent != NULL) {
          bool writable = pagedir_is_writable(pd_parent, vaddr);
          
          void *kpage_child = palloc_get_page(PAL_USER);
          if (kpage_child == NULL) {
            success = false;
            break;
          }
          
          memcpy(kpage_child, kpage_parent, PGSIZE);
          
          if (!pagedir_set_page(new_pcb->pagedir, vaddr, kpage_child, writable)) {
            palloc_free_page(kpage_child);
            success = false;
            break;
          }
        }
      }

      /* If the copy fails midway, release any user pages that were already allocated. */
      if (!success) {
        for (vaddr = 0; vaddr < (uint32_t *)PHYS_BASE; vaddr += PGSIZE / sizeof(uint32_t)) {
          void *kpage_child = pagedir_get_page(new_pcb->pagedir, vaddr);
          if (kpage_child != NULL) {
            palloc_free_page(kpage_child);
          }
        }
      }
    }
  }

  /* Duplicate the file descriptor table. */
  if (success) {
    new_pcb->next_fd = parent->pcb->next_fd;
    
    /* Protect descriptor duplication with the filesystem lock while multiple children are forking. */
    lock_acquire(&filesys_lock);
    for (int i = 2; i < 128; i++) {
      if (parent->pcb->fd_table[i] != NULL) {
        new_pcb->fd_table[i] = parent->pcb->fd_table[i];
        file_ref_increment(new_pcb->fd_table[i]);
      } else {
        new_pcb->fd_table[i] = NULL;
      }
    }
    lock_release(&filesys_lock);
  }

  /* Prepare the interrupt frame for user-space execution. */
  if (success) {
    local_if = args->parent_if;
    /* The child always returns 0 from fork. */
    local_if.eax = 0; 
  }

  /* Link the child to its synchronization structure. */
  cur->child_frame = args->child_frame;
  if (cur->child_frame != NULL) {
    cur->child_frame->load_success = success;
    sema_up(&cur->child_frame->load_sema);
  }

  /* Release the temporary fork arguments. */
  free(args);

  if (!success) {
    if (cur->pcb != NULL) {
      if (cur->pcb->pagedir != NULL) {
        pagedir_destroy(cur->pcb->pagedir);
      }
      free(cur->pcb);
      cur->pcb = NULL;
    }
    thread_exit();
  }

  /* Jump back to user mode with the restored stack pointer. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&local_if) : "memory");
  NOT_REACHED();
}

/* Implementation of process_fork. */
pid_t process_fork(struct intr_frame *f) {
  struct thread *cur = thread_current();
  
  /* Allocate the child tracking structure. */
  struct child_process *child = malloc(sizeof(struct child_process));
  if (child == NULL)
    return TID_ERROR;

  sema_init(&child->load_sema, 0);
  sema_init(&child->exit_sema, 0);
  child->load_success = false;
  child->exit_status = -1;
  child->exited = false;
  child->waited = false;

  /* Package the thread creation arguments. */
  struct fork_args *args = malloc(sizeof(struct fork_args));
  if (args == NULL) {
    free(child);
    return TID_ERROR;
  }
  args->parent = cur;
  args->parent_if = *f; /* Save the full register state of the parent. */
  args->child_frame = child;

  /* Create the child thread using the parent's name. */
  tid_t tid = thread_create(cur->pcb->process_name, PRI_DEFAULT, fork_thread_entry, args);
  if (tid == TID_ERROR) {
    free(args);
    free(child);
    return TID_ERROR;
  }

  child->pid = (int)tid;
  list_push_back(&cur->pcb->children, &child->elem);

  /* Wait until the child has finished copying the parent's memory. */
  sema_down(&child->load_sema);

  if (!child->load_success) {
    return TID_ERROR;
  }

  /* Return the child's PID to the parent. */
  return tid;
}

/* Start a new user process from an exec request. */
static void start_process(void* args_) {
  struct exec_args* args = (struct exec_args*)args_;
  char* file_name = args->file_name;
  struct child_process* child_frame = args->child;
  
  struct thread* t = thread_current();
  t->child_frame = child_frame;
  struct intr_frame if_;
  bool success, pcb_success;

  char *token, *save_ptr;
  char *argv[64]; 
  int argc = 0;

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;

  /* Initialize process control block */
  if (success) {
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;
    list_init(&new_pcb->children);

    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->pcb->process_name);
    t->pcb->executable = NULL;
    t->pcb->exit_status = 0;
    t->pcb->load_success = true;

    new_pcb->next_fd = 2; /* File descriptors 0 and 1 are reserved for stdin/stdout. */
    for (int i = 0; i < 128; i++) {
        new_pcb->fd_table[i] = NULL;
    }
  }

  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;

    for (token = strtok_r(file_name, " ", &save_ptr); token != NULL;
         token = strtok_r(NULL, " ", &save_ptr)) {
      argv[argc] = token;
      argc++;
    }

    success = load(argv[0], &if_.eip, &if_.esp);
    
    /* Wake the parent regardless of the outcome. */
    if (child_frame != NULL) {
      child_frame->load_success = success;
      sema_up(&child_frame->load_sema);
    }
    
    if (success) {
      uintptr_t arg_address[64];

      /* Push the actual argument strings onto the stack. */
      for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        if_.esp -= len;
        memcpy(if_.esp, argv[i], len);
        arg_address[i] = (uintptr_t)if_.esp;
      }

      /* Maintain the strict 16-byte stack alignment required by the i386 ABI. */
      size_t overhead = (argc + 4) * 4;
      uintptr_t target = (uintptr_t)if_.esp - overhead - 12;
      size_t padding = target % 16;
      
      if (padding > 0) {
        if_.esp = (void *)((uintptr_t)if_.esp - padding);
        memset(if_.esp, 0, padding);
      }

      /* Push the argv null sentinel. */
      if_.esp -= sizeof(char *);
      *(char **)if_.esp = NULL;

      /* Push the addresses of the argument strings. */
      for (int i = argc - 1; i >= 0; i--) {
        if_.esp -= sizeof(char *);
        *(uintptr_t *)if_.esp = arg_address[i];
      }

      /* Push the pointer to the argv array. */
      char **argv_ptr = (char **)if_.esp;
      if_.esp -= sizeof(char **);
      *(char ***)if_.esp = argv_ptr;

      /* Push the argc value. */
      if_.esp -= sizeof(int);
      *(int *)if_.esp = argc;

      /* Push a fake return address. */
      if_.esp -= sizeof(void (*)(void));
      *(void **)if_.esp = NULL;
    }
  }

  if (!success && pcb_success) {
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
  }

  /* Clean up the temporary exec wrapper. */
  palloc_free_page(file_name);
  free(args);

  if (!success) {
    thread_exit();
  }

  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid UNUSED) {
  struct process* current_pcb = thread_current()->pcb;
  if (current_pcb == NULL)
    return -1;

  struct list_elem* e;
  struct child_process* child = NULL;

  for (e = list_begin(&current_pcb->children); e != list_end(&current_pcb->children);
       e = list_next(e)) {
    struct child_process* candidate = list_entry(e, struct child_process, elem);
    if (candidate->pid == child_pid) {
      child = candidate;
      break;
    }
  }

  if (child == NULL || child->waited)
    return -1;

  child->waited = true;
  if (!child->exited)
    sema_down(&child->exit_sema);

  int status = child->exit_status;
  list_remove(&child->elem);
  free(child);
  return status;
}

/* Free the current process's resources. */
void process_exit(void) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }

  /* Close all open file descriptors owned by the process. */
  for (int i = 2; i < 128; i++) {
    if (cur->pcb->fd_table[i] != NULL) {
      file_ref_decrement_and_close(cur->pcb->fd_table[i]);
      cur->pcb->fd_table[i] = NULL;
    }
  }

  /* Release and close the executable file if it was marked write-restricted. */
  if (cur->pcb->executable != NULL) {
    file_allow_write(cur->pcb->executable); 
    file_close(cur->pcb->executable);       
    cur->pcb->executable = NULL;
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  /* Keep a local reference to the child synchronization structure before freeing the PCB. */
  struct child_process* cf = cur->child_frame;
  int final_exit_status = cur->pcb->exit_status;

  /* Free the PCB memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  free(pcb_to_free);

  /* Wake the parent after the current thread has fully exited and its resources are released. */
  if (cf != NULL) {
    cf->exit_status = final_exit_status;
    cf->exited = true;
    sema_up(&cf->exit_sema);
  }

  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* file_name, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  lock_acquire(&filesys_lock);

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Open executable file. */
  file = filesys_open(file_name);
  if (file == NULL) {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }
  file_deny_write(file);
  t->pcb->executable = file;

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  if (!success) {
    if (file != NULL) {
      file_close(file);
      if (t->pcb != NULL)
        t->pcb->executable = NULL;
    }
  }
  lock_release(&filesys_lock);
  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp) {
  uint8_t* kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true);
    if (success)
      *esp = PHYS_BASE;
    else
      palloc_free_page(kpage);
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void) UNUSED, void** esp UNUSED) { return false; }

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf UNUSED, pthread_fun tf UNUSED, void* arg UNUSED) { return -1; }

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_ UNUSED) {}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid UNUSED) { return -1; }

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {}
