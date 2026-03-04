#ifndef _PROC_H_
#define _PROC_H_

#include "riscv.h"
#include "config.h"

typedef struct trapframe_t {
  /* offset:0   */ riscv_regs regs;
  /* offset:248 */ uint64 kernel_sp;
  /* offset:256 */ uint64 kernel_trap;
  /* offset:264 */ uint64 epc;
}trapframe;

typedef struct process_t {
  uint64 kstack;
  trapframe* trapframe;
}process;

void switch_to(process*);

extern process* current[NCPU];

#endif
