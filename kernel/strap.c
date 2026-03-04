/*
 * Utility functions for trap handling in Supervisor mode.
 */

#include "riscv.h"
#include "config.h"
#include "process.h"
#include "strap.h"
#include "syscall.h"

#include "spike_interface/spike_utils.h"

static void handle_syscall(trapframe *tf) {
  tf->epc += 4;
  tf->regs.a0 = do_syscall(tf->regs.a0, tf->regs.a1, tf->regs.a2, tf->regs.a3,
                           tf->regs.a4, tf->regs.a5, tf->regs.a6, tf->regs.a7);
}

static uint64 g_ticks[NCPU] = {0};

void handle_mtimer_trap() {
  int hartid = (int)read_tp();
  g_ticks[hartid] += 1;
  write_csr(sip, read_csr(sip) & ~SIP_SSIP);
}

void smode_trap_handler(void) {
  if ((read_csr(sstatus) & SSTATUS_SPP) != 0) panic("usertrap: not from user mode");

  int hartid = (int)read_tp();
  process *p = current[hartid];
  assert(p);

  p->trapframe->epc = read_csr(sepc);

  uint64 cause = read_csr(scause);

  if (cause == CAUSE_USER_ECALL) {
    handle_syscall(p->trapframe);
  } else if (cause == CAUSE_MTIMER_S_TRAP) {
    handle_mtimer_trap();
  } else {
    sprint("hartid = %d: smode_trap_handler(): unexpected scause %p\n", hartid, cause);
    sprint("hartid = %d:             sepc=%p stval=%p\n", hartid, read_csr(sepc), read_csr(stval));
    panic("unexpected exception happened.\n");
  }

  switch_to(p);
}
