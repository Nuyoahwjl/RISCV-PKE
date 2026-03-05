/*
 * Supervisor-mode startup codes
 */

#include "riscv.h"
#include "string.h"
#include "elf.h"
#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "memlayout.h"
#include "config.h"
#include "spike_interface/spike_utils.h"

// One user process per hart in lab2_challenge3.
process user_app[NCPU];

// Set to 1 by hart0 after pmm/kern_vm init completes. Other harts wait on it.
// (Used by M-mode m_start as well to align output ordering.)
volatile int s_mem_init_done = 0;

extern char trap_sec_start[];

static void enable_paging() {
  write_csr(satp, MAKE_SATP(g_kernel_pagetable));
  flush_tlb();
}

void load_user_program(process *proc) {
  int hartid = (int)read_tp();

  sprint("hartid = %d: User application is loading.\n", hartid);

  proc->trapframe = (trapframe *)alloc_page();
  memset(proc->trapframe, 0, sizeof(trapframe));

  proc->pagetable = (pagetable_t)alloc_page();
  memset((void *)proc->pagetable, 0, PGSIZE);

  proc->kstack = (uint64)alloc_page() + PGSIZE;   // user kernel stack top
  uint64 user_stack = (uint64)alloc_page();       // physical address of user stack bottom

  proc->trapframe->regs.sp = USER_STACK_TOP;      // user stack top (VA)
  proc->trapframe->regs.tp = (uint64)hartid;      // keep hartid in tp across U/S

  sprint("hartid = %d: user frame 0x%lx, user stack 0x%lx, user kstack 0x%lx \n",
         hartid, (uint64)proc->trapframe, proc->trapframe->regs.sp, proc->kstack);

  // each hart/process has its own simple-heap virtual address allocator.
  g_ufree_page[hartid] = USER_FREE_ADDRESS_START;

  load_bincode_from_host_elf(proc);

  user_vm_map((pagetable_t)proc->pagetable, USER_STACK_TOP - PGSIZE, PGSIZE, user_stack,
              prot_to_type(PROT_WRITE | PROT_READ, 1));

  user_vm_map((pagetable_t)proc->pagetable, (uint64)proc->trapframe, PGSIZE, (uint64)proc->trapframe,
              prot_to_type(PROT_WRITE | PROT_READ, 0));

  user_vm_map((pagetable_t)proc->pagetable, (uint64)trap_sec_start, PGSIZE, (uint64)trap_sec_start,
              prot_to_type(PROT_READ | PROT_EXEC, 0));
}

int s_start(void) {
  int hartid = (int)read_tp();
  sprint("hartid = %d: Enter supervisor mode...\n", hartid);

  write_csr(satp, 0);

  if (hartid == 0) {
    pmm_init();
    kern_vm_init();
    enable_paging();

    asm volatile("fence rw, rw" ::: "memory");
    s_mem_init_done = 1;
  } else {
    while (!s_mem_init_done) asm volatile("nop");
    enable_paging();
  }

  load_user_program(&user_app[hartid]);

  sprint("hartid = %d: Switch to user mode...\n", hartid);
  switch_to(&user_app[hartid]);

  return 0;
}
