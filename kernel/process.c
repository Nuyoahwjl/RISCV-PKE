/*
 * Utility functions for process management.
 *
 * In lab1_challenge3, each hart runs its own user process, so we keep one "current"
 * pointer per hart.
 */

#include "riscv.h"
#include "strap.h"
#include "config.h"
#include "process.h"
#include "elf.h"
#include "string.h"

#include "spike_interface/spike_utils.h"

extern char smode_trap_vector[];
extern void return_to_user(trapframe*);

process* current[NCPU] = {0};

void switch_to(process* proc) {
  assert(proc);
  int hartid = (int)read_tp();
  current[hartid] = proc;

  write_csr(stvec, (uint64)smode_trap_vector);

  proc->trapframe->kernel_sp = proc->kstack;
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;

  unsigned long x = read_csr(sstatus);
  x &= ~SSTATUS_SPP;
  x |= SSTATUS_SPIE;
  write_csr(sstatus, x);

  write_csr(sepc, proc->trapframe->epc);

  return_to_user(proc->trapframe);
}
