/*
 * Supervisor-mode startup codes
 */

#include "riscv.h"
#include "string.h"
#include "elf.h"
#include "process.h"
#include "config.h"

#include "spike_interface/spike_utils.h"

// In lab1_challenge3 we run one user app per hart.
process user_app[NCPU];

// user apps are linked at different bases (see user0.lds/user1.lds). Use the same stride
// to separate per-hart fixed addresses (stack/kstack/trapframe) in Bare mode.
#define USER_ADDR_STRIDE 0x4000000UL

void load_user_program(process *proc) {
  int hartid = (int)read_tp();

  proc->trapframe = (trapframe *)(USER_TRAP_FRAME + (uint64)hartid * USER_ADDR_STRIDE);
  memset(proc->trapframe, 0, sizeof(trapframe));

  proc->kstack = (uint64)(USER_KSTACK + (uint64)hartid * USER_ADDR_STRIDE);

  proc->trapframe->regs.sp = (uint64)(USER_STACK + (uint64)hartid * USER_ADDR_STRIDE);

  // keep hartid in tp in both S/U mode
  proc->trapframe->regs.tp = (uint64)hartid;

  load_bincode_from_host_elf(proc);
}

int s_start(void) {
  int hartid = (int)read_tp();
  sprint("hartid = %d: Enter supervisor mode...\n", hartid);

  write_csr(satp, 0);

  load_user_program(&user_app[hartid]);

  sprint("hartid = %d: Switch to user mode...\n", hartid);

  switch_to(&user_app[hartid]);

  return 0;
}
