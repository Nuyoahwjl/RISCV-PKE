#ifndef _PROC_H_
#define _PROC_H_

#include "riscv.h"
#include "config.h"

typedef struct trapframe_t {
  /* offset:0   */ riscv_regs regs;
  /* offset:248 */ uint64 kernel_sp;
  /* offset:256 */ uint64 kernel_trap;
  /* offset:264 */ uint64 epc;
  /* offset:272 */ uint64 kernel_satp;
}trapframe;

typedef struct process_t {
  uint64 kstack;
  pagetable_t pagetable;
  trapframe* trapframe;
}process;

void switch_to(process*);

// one current process per hart
extern process* current[NCPU];

// per-hart simple heap cursor
extern uint64 g_ufree_page[NCPU];

#endif
