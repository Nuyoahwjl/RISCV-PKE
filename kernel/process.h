#ifndef _PROC_H_
#define _PROC_H_

#include "riscv.h"
#include "proc_file.h"

typedef struct trapframe_t {
  // space to store context (all common registers)
  /* offset:0   */ riscv_regs regs;

  // process's "user kernel" stack
  /* offset:248 */ uint64 kernel_sp;
  // pointer to smode_trap_handler
  /* offset:256 */ uint64 kernel_trap;
  // saved user process counter
  /* offset:264 */ uint64 epc;

  // kernel page table. added @lab2_1
  /* offset:272 */ uint64 kernel_satp;
} trapframe;

// code file struct, including directory index and file name char pointer
typedef struct {
  uint64 dir;
  char *file;
} code_file;

// address-line number-file name table
typedef struct {
  uint64 addr;
  uint64 line;
  uint64 file;
} addr_line;

// riscv-pke kernel supports at most 32 processes
#define NPROC 32
// maximum number of pages in a process's heap
#define MAX_HEAP_PAGES 32

// possible status of a process
enum proc_status {
  FREE,
  READY,
  RUNNING,
  BLOCKED,
  ZOMBIE,
};

// types of a segment
enum segment_type {
  STACK_SEGMENT = 0,
  CONTEXT_SEGMENT,
  SYSTEM_SEGMENT,
  HEAP_SEGMENT,
  CODE_SEGMENT,
  DATA_SEGMENT,
};

// the VM regions mapped to a user process
typedef struct mapped_region {
  uint64 va;
  uint32 npages;
  uint32 seg_type;
} mapped_region;

typedef struct process_heap_manager {
  uint64 heap_top;
  uint64 heap_bottom;

  uint64 free_pages_address[MAX_HEAP_PAGES];
  uint32 free_pages_count;
} process_heap_manager;

// the extremely simple definition of process, used for begining labs of PKE
typedef struct process_t {
  // pointing to the stack used in trap handling.
  uint64 kstack;
  // user page table
  pagetable_t pagetable;
  // trapframe storing the context of a (User mode) process.
  trapframe *trapframe;

  // debug line mapping for error-line challenge
  char *debugline;
  char **dir;
  code_file *file;
  addr_line *line;
  int line_ind;

  // points to a page that contains mapped_regions. below are added @lab3_1
  mapped_region *mapped_info;
  // next free mapped region in mapped_info
  int total_mapped_region;

  // heap management
  process_heap_manager user_heap;

  // better_malloc/better_free metadata stored in user heap address space
  uint64 heap_head;
  uint64 heap_tail;
  uint64 heap_end;

  // process id
  uint64 pid;
  // process status
  int status;
  // parent process
  struct process_t *parent;
  // next queue element
  struct process_t *queue_next;

  // accounting. added @lab3_3
  int tick_count;

  // file system. added @lab4_1
  proc_file_management *pfiles;
} process;

// switch to run user app
void switch_to(process *);

// initialize process pool (the procs[] array)
void init_proc_pool();
// allocate an empty process, init its vm space. returns its pid
process *alloc_process();
// reclaim a process, destruct its vm space and free physical pages.
int free_process(process *proc);
// fork a child from parent
int do_fork(process *parent);
int do_wait(int pid);
int do_exec(process *proc, const char *path, const char *arg);

// current running process
extern process *current;

#endif
