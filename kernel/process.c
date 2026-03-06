/*
 * Utility functions for process management. 
 *
 * Note: in Lab1, only one process (i.e., our user application) exists. Therefore, 
 * PKE OS at this stage will set "current" to the loaded user application, and also
 * switch to the old "current" process after trap handling.
 */

#include "riscv.h"
#include "strap.h"
#include "config.h"
#include "process.h"
#include "elf.h"
#include "string.h"
#include "vmm.h"
#include "pmm.h"
#include "memlayout.h"
#include "sched.h"
#include "spike_interface/spike_utils.h"

//Two functions defined in kernel/usertrap.S
extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);

// trap_sec_start points to the beginning of S-mode trap segment (i.e., the entry point
// of S-mode trap vector).
extern char trap_sec_start[];

// process pool. added @lab3_1
process procs[NPROC];

// current points to the currently running user-mode application.
process* current = NULL;

//
// switch to a user-mode process
//
void switch_to(process* proc) {
  assert(proc);
  current = proc;

  // write the smode_trap_vector (64-bit func. address) defined in kernel/strap_vector.S
  // to the stvec privilege register, such that trap handler pointed by smode_trap_vector
  // will be triggered when an interrupt occurs in S mode.
  write_csr(stvec, (uint64)smode_trap_vector);

  // set up trapframe values (in process structure) that smode_trap_vector will need when
  // the process next re-enters the kernel.
  proc->trapframe->kernel_sp = proc->kstack;      // process's kernel stack
  proc->trapframe->kernel_satp = read_csr(satp);  // kernel page table
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;

  // SSTATUS_SPP and SSTATUS_SPIE are defined in kernel/riscv.h
  // set S Previous Privilege mode (the SSTATUS_SPP bit in sstatus register) to User mode.
  unsigned long x = read_csr(sstatus);
  x &= ~SSTATUS_SPP;  // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE;  // enable interrupts in user mode

  // write x back to 'sstatus' register to enable interrupts, and sret destination mode.
  write_csr(sstatus, x);

  // set S Exception Program Counter (sepc register) to the elf entry pc.
  write_csr(sepc, proc->trapframe->epc);

  // make user page table. macro MAKE_SATP is defined in kernel/riscv.h. added @lab2_1
  uint64 user_satp = MAKE_SATP(proc->pagetable);

  // return_to_user() is defined in kernel/strap_vector.S. switch to user mode with sret.
  // note, return_to_user takes two parameters @ and after lab2_1.
  return_to_user(proc->trapframe, user_satp);
}

//
// initialize process pool (the procs[] array). added @lab3_1
//
void init_proc_pool() {
  memset( procs, 0, sizeof(process)*NPROC );

  for (int i = 0; i < NPROC; ++i) {
    procs[i].status = FREE;
    procs[i].pid = i;
  }
}

//
// allocate an empty process, init its vm space. returns the pointer to
// process strcuture. added @lab3_1
//
process* alloc_process() {
  // locate the first usable process structure
  int i;

  for( i=0; i<NPROC; i++ )
    if( procs[i].status == FREE ) break;

  if( i>=NPROC ){
    panic( "cannot find any free process structure.\n" );
    return 0;
  }

  // init proc[i]'s vm space
  procs[i].trapframe = (trapframe *)alloc_page();  //trapframe, used to save context
  memset(procs[i].trapframe, 0, sizeof(trapframe));

  // page directory
  procs[i].pagetable = (pagetable_t)alloc_page();
  memset((void *)procs[i].pagetable, 0, PGSIZE);

  procs[i].kstack = (uint64)alloc_page() + PGSIZE;   //user kernel stack top
  uint64 user_stack = (uint64)alloc_page();       //phisical address of user stack bottom
  procs[i].trapframe->regs.sp = USER_STACK_TOP;  //virtual address of user stack top

  // allocates a page to record memory regions (segments)
  procs[i].mapped_info = (mapped_region*)alloc_page();
  memset( procs[i].mapped_info, 0, PGSIZE );

  // map user stack in userspace
  user_vm_map((pagetable_t)procs[i].pagetable, USER_STACK_TOP - PGSIZE, PGSIZE,
    user_stack, prot_to_type(PROT_WRITE | PROT_READ, 1));
  procs[i].mapped_info[STACK_SEGMENT].va = USER_STACK_TOP - PGSIZE;
  procs[i].mapped_info[STACK_SEGMENT].npages = 1;
  procs[i].mapped_info[STACK_SEGMENT].seg_type = STACK_SEGMENT;

  // map trapframe in user space (direct mapping as in kernel space).
  user_vm_map((pagetable_t)procs[i].pagetable, (uint64)procs[i].trapframe, PGSIZE,
    (uint64)procs[i].trapframe, prot_to_type(PROT_WRITE | PROT_READ, 0));
  procs[i].mapped_info[CONTEXT_SEGMENT].va = (uint64)procs[i].trapframe;
  procs[i].mapped_info[CONTEXT_SEGMENT].npages = 1;
  procs[i].mapped_info[CONTEXT_SEGMENT].seg_type = CONTEXT_SEGMENT;

  // map S-mode trap vector section in user space (direct mapping as in kernel space)
  // we assume that the size of usertrap.S is smaller than a page.
  user_vm_map((pagetable_t)procs[i].pagetable, (uint64)trap_sec_start, PGSIZE,
    (uint64)trap_sec_start, prot_to_type(PROT_READ | PROT_EXEC, 0));
  procs[i].mapped_info[SYSTEM_SEGMENT].va = (uint64)trap_sec_start;
  procs[i].mapped_info[SYSTEM_SEGMENT].npages = 1;
  procs[i].mapped_info[SYSTEM_SEGMENT].seg_type = SYSTEM_SEGMENT;

  sprint("in alloc_proc. user frame 0x%lx, user stack 0x%lx, user kstack 0x%lx \n",
    procs[i].trapframe, procs[i].trapframe->regs.sp, procs[i].kstack);

  // initialize the process's heap manager
  procs[i].user_heap.heap_top = USER_FREE_ADDRESS_START;
  procs[i].user_heap.heap_bottom = USER_FREE_ADDRESS_START;
  procs[i].user_heap.free_pages_count = 0;

  // map user heap in userspace
  procs[i].mapped_info[HEAP_SEGMENT].va = USER_FREE_ADDRESS_START;
  procs[i].mapped_info[HEAP_SEGMENT].npages = 0;  // no pages are mapped to heap yet.
  procs[i].mapped_info[HEAP_SEGMENT].seg_type = HEAP_SEGMENT;

  procs[i].total_mapped_region = 4;

  // initialize files_struct
  procs[i].pfiles = init_proc_file_management();
  sprint("in alloc_proc. build proc_file_management successfully.\n");

  // return after initialization.
  return &procs[i];
}

//
// reclaim a process. added @lab3_1
//
int free_process( process* proc ) {
  // we set the status to ZOMBIE, but cannot destruct its vm space immediately.
  // since proc can be current process, and its user kernel stack is currently in use!
  // but for proxy kernel, it (memory leaking) may NOT be a really serious issue,
  // as it is different from regular OS, which needs to run 7x24.
  proc->status = ZOMBIE;

  return 0;
}

//
// implements fork syscal in kernel. added @lab3_1
// basic idea here is to first allocate an empty process (child), then duplicate the
// context and data segments of parent process to the child, and lastly, map other
// segments (code, system) of the parent to child. the stack segment remains unchanged
// for the child.
//
int do_fork(process* parent) {
  sprint("will fork a child from parent %d.\n", parent->pid);
  process* child = alloc_process();

  for (int i = 0; i < parent->total_mapped_region; i++) {
    switch (parent->mapped_info[i].seg_type) {
      case CONTEXT_SEGMENT:
        *child->trapframe = *parent->trapframe;
        break;

      case STACK_SEGMENT:
        memcpy((void*)lookup_pa(child->pagetable, child->mapped_info[STACK_SEGMENT].va),
               (void*)lookup_pa(parent->pagetable, parent->mapped_info[i].va),
               PGSIZE);
        break;

      case HEAP_SEGMENT: {
        int free_block_filter[MAX_HEAP_PAGES];
        memset(free_block_filter, 0, sizeof(free_block_filter));

        uint64 heap_bottom = parent->user_heap.heap_bottom;
        for (int j = 0; j < parent->user_heap.free_pages_count; j++) {
          int index =
              (parent->user_heap.free_pages_address[j] - heap_bottom) / PGSIZE;
          free_block_filter[index] = 1;
        }

        for (uint64 heap_block = parent->user_heap.heap_bottom;
             heap_block < parent->user_heap.heap_top;
             heap_block += PGSIZE) {
          if (free_block_filter[(heap_block - heap_bottom) / PGSIZE])
            continue;

          void* child_pa = alloc_page();
          memcpy(child_pa, (void*)lookup_pa(parent->pagetable, heap_block), PGSIZE);
          user_vm_map((pagetable_t)child->pagetable,
                      heap_block,
                      PGSIZE,
                      (uint64)child_pa,
                      prot_to_type(PROT_WRITE | PROT_READ, 1));
        }

        child->mapped_info[HEAP_SEGMENT].npages =
            parent->mapped_info[HEAP_SEGMENT].npages;
        memcpy((void*)&child->user_heap,
               (void*)&parent->user_heap,
               sizeof(parent->user_heap));
        break;
      }

      case CODE_SEGMENT:
        map_pages(child->pagetable,
                  parent->mapped_info[i].va,
                  parent->mapped_info[i].npages * PGSIZE,
                  lookup_pa(parent->pagetable, parent->mapped_info[i].va),
                  prot_to_type(PROT_EXEC | PROT_READ, 1));

        child->mapped_info[child->total_mapped_region].va =
            parent->mapped_info[i].va;
        child->mapped_info[child->total_mapped_region].npages =
            parent->mapped_info[i].npages;
        child->mapped_info[child->total_mapped_region].seg_type = CODE_SEGMENT;
        child->total_mapped_region++;
        break;

      case DATA_SEGMENT: {
        uint64 start_va = parent->mapped_info[i].va;
        int pages = parent->mapped_info[i].npages;

        for (int k = 0; k < pages; k++) {
          uint64 va = start_va + (uint64)k * PGSIZE;
          uint64 old_pa = lookup_pa(parent->pagetable, va);

          char *pa_copy = alloc_page();
          memcpy(pa_copy, (void *)old_pa, PGSIZE);

          map_pages(child->pagetable,
                    va,
                    PGSIZE,
                    (uint64)pa_copy,
                    prot_to_type(PROT_READ | PROT_WRITE, 1));
        }

        child->mapped_info[child->total_mapped_region].va =
            parent->mapped_info[i].va;
        child->mapped_info[child->total_mapped_region].npages =
            parent->mapped_info[i].npages;
        child->mapped_info[child->total_mapped_region].seg_type = DATA_SEGMENT;
        child->total_mapped_region++;
        
        sprint( "do_fork map code segment at pa:%lx of parent to child at va:%lx.\n",
          lookup_pa(parent->pagetable, parent->mapped_info[i].va),
          parent->mapped_info[i].va );
        
        break;
      }
    }
  }

  child->status = READY;
  child->trapframe->regs.a0 = 0;
  child->parent = parent;
  insert_to_ready_queue(child);

  return child->pid;
}

int do_wait(int pid) {
  while (1) {
    int has_child = 0;

    for (int i = 0; i < NPROC; i++) {
      process *cp = &procs[i];

      if (cp->parent != current)
        continue;

      if (pid != -1 && cp->pid != (uint64)pid)
        continue;

      has_child = 1;
      if (cp->status == ZOMBIE) {
        cp->status = FREE;
        return cp->pid;
      }
    }

    if (!has_child)
      return -1;

    insert_to_blocked_queue(current);
    schedule();
  }
}

int do_exec(process *proc, const char *path, const char *arg) {
  if (proc == NULL || path == NULL) return -1;

  char kpath[MAX_PATH_LEN];
  char karg[MAX_PATH_LEN];
  memset(kpath, 0, sizeof(kpath));
  memset(karg, 0, sizeof(karg));
  strcpy(kpath, path);
  if (arg) strcpy(karg, arg);

  // 用临时 process 构造新的用户地址空间
  process newp;
  memset(&newp, 0, sizeof(newp));

  // 保留当前进程的“身份”和文件管理结构
  newp.pid = proc->pid;
  newp.kstack = proc->kstack;
  newp.parent = proc->parent;
  newp.pfiles = proc->pfiles;
  newp.status = proc->status;

  // trapframe
  newp.trapframe = (trapframe *)alloc_page();
  memset(newp.trapframe, 0, sizeof(trapframe));

  // pagetable
  newp.pagetable = (pagetable_t)alloc_page();
  memset((void *)newp.pagetable, 0, PGSIZE);

  // mapped_info
  newp.mapped_info = (mapped_region *)alloc_page();
  memset(newp.mapped_info, 0, PGSIZE);

  // user stack
  uint64 user_stack_pa = (uint64)alloc_page();
  newp.trapframe->regs.sp = USER_STACK_TOP;
  user_vm_map((pagetable_t)newp.pagetable,
              USER_STACK_TOP - PGSIZE,
              PGSIZE,
              user_stack_pa,
              prot_to_type(PROT_WRITE | PROT_READ, 1));
  newp.mapped_info[STACK_SEGMENT].va = USER_STACK_TOP - PGSIZE;
  newp.mapped_info[STACK_SEGMENT].npages = 1;
  newp.mapped_info[STACK_SEGMENT].seg_type = STACK_SEGMENT;

  // trapframe direct mapping
  user_vm_map((pagetable_t)newp.pagetable,
              (uint64)newp.trapframe,
              PGSIZE,
              (uint64)newp.trapframe,
              prot_to_type(PROT_WRITE | PROT_READ, 0));
  newp.mapped_info[CONTEXT_SEGMENT].va = (uint64)newp.trapframe;
  newp.mapped_info[CONTEXT_SEGMENT].npages = 1;
  newp.mapped_info[CONTEXT_SEGMENT].seg_type = CONTEXT_SEGMENT;

  // trap vector section
  user_vm_map((pagetable_t)newp.pagetable,
              (uint64)trap_sec_start,
              PGSIZE,
              (uint64)trap_sec_start,
              prot_to_type(PROT_READ | PROT_EXEC, 0));
  newp.mapped_info[SYSTEM_SEGMENT].va = (uint64)trap_sec_start;
  newp.mapped_info[SYSTEM_SEGMENT].npages = 1;
  newp.mapped_info[SYSTEM_SEGMENT].seg_type = SYSTEM_SEGMENT;

  // heap manager
  newp.user_heap.heap_top = USER_FREE_ADDRESS_START;
  newp.user_heap.heap_bottom = USER_FREE_ADDRESS_START;
  newp.user_heap.free_pages_count = 0;
  newp.mapped_info[HEAP_SEGMENT].va = USER_FREE_ADDRESS_START;
  newp.mapped_info[HEAP_SEGMENT].npages = 0;
  newp.mapped_info[HEAP_SEGMENT].seg_type = HEAP_SEGMENT;

  newp.total_mapped_region = 4;

  // 装入新的 ELF
  if (load_bincode_from_path(&newp, kpath) < 0) {
    sprint("do_exec: load_bincode_from_path failed.\n");
    return -1;
  }

  // 传一个参数给新程序：argc=1, argv[0]=arg
  if (arg != NULL) {
    uint64 arg_page_va = USER_FREE_ADDRESS_START;
    uint64 arg_page_pa = (uint64)alloc_page();
    memset((void *)arg_page_pa, 0, PGSIZE);

    user_vm_map((pagetable_t)newp.pagetable,
                arg_page_va,
                PGSIZE,
                arg_page_pa,
                prot_to_type(PROT_WRITE | PROT_READ, 1));

    char *arg_str_va = (char *)arg_page_va;
    uint64 *argv_va = (uint64 *)(arg_page_va + 128);

    char *arg_page_kva =
        (char *)lookup_pa((pagetable_t)newp.pagetable, arg_page_va);

    strcpy(arg_page_kva, karg);
    ((uint64 *)(arg_page_kva + 128))[0] = (uint64)arg_str_va;
    ((uint64 *)(arg_page_kva + 128))[1] = 0;

    newp.trapframe->regs.a0 = 1;              // argc
    newp.trapframe->regs.a1 = (uint64)argv_va; // argv

    // 占掉 heap 首页，避免后续 naive_malloc 覆盖参数页
    newp.user_heap.heap_top += PGSIZE;
    newp.mapped_info[HEAP_SEGMENT].npages = 1;
  } else {
    newp.trapframe->regs.a0 = 0;
    newp.trapframe->regs.a1 = 0;
  }

  // 提交新的用户地址空间
  proc->pagetable = newp.pagetable;
  proc->trapframe = newp.trapframe;
  proc->mapped_info = newp.mapped_info;
  proc->total_mapped_region = newp.total_mapped_region;
  proc->user_heap = newp.user_heap;
  proc->tick_count = 0;

  return 0;
}
