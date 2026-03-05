/*
 * Utility functions for process management.
 *
 * In lab2_challenge3_multicoremem, each hart runs its own user process. We therefore
 * maintain one "current" pointer and one simple-heap cursor per hart.
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
#include "spike_interface/spike_utils.h"

extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);

process* current[NCPU] = {0};
uint64 g_ufree_page[NCPU] = {0};

void switch_to(process* proc) {
  assert(proc);
  int hartid = (int)read_tp();
  current[hartid] = proc;

  // mark that subsequent alloc_page() calls are from user context on this hart, for printing.
  vm_alloc_stage[hartid] = 1;

  write_csr(stvec, (uint64)smode_trap_vector);

  proc->trapframe->kernel_sp = proc->kstack;
  proc->trapframe->kernel_satp = read_csr(satp);
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;

  unsigned long x = read_csr(sstatus);
  x &= ~SSTATUS_SPP;
  x |= SSTATUS_SPIE;
  write_csr(sstatus, x);

  write_csr(sepc, proc->trapframe->epc);

  uint64 user_satp = MAKE_SATP(proc->pagetable);

  return_to_user(proc->trapframe, user_satp);
}
