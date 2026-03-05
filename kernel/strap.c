/*
 * Utility functions for trap handling in Supervisor mode.
 */

#include "riscv.h"
#include "config.h"
#include "process.h"
#include "strap.h"
#include "syscall.h"
#include "pmm.h"
#include "vmm.h"
#include "util/functions.h"

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

void handle_user_page_fault(uint64 scause, uint64 sepc, uint64 stval) {
  (void)sepc;
  sprint("handle_page_fault: %lx\n", stval);

  int hartid = (int)read_tp();
  process *p = current[hartid];
  assert(p);

  switch (scause) {
    case CAUSE_STORE_PAGE_FAULT:
    case CAUSE_LOAD_PAGE_FAULT:
      map_pages(p->pagetable, ROUNDDOWN(stval, PGSIZE), PGSIZE, (uint64)alloc_page(),
                prot_to_type(PROT_READ | PROT_WRITE, 1));
      break;
    default:
      sprint("unknown page fault.\n");
      break;
  }
}

void smode_trap_handler(void) {
  if ((read_csr(sstatus) & SSTATUS_SPP) != 0) panic("usertrap: not from user mode");

  int hartid = (int)read_tp();
  process *p = current[hartid];
  assert(p);

  p->trapframe->epc = read_csr(sepc);

  uint64 cause = read_csr(scause);

  switch (cause) {
    case CAUSE_USER_ECALL:
      handle_syscall(p->trapframe);
      break;
    case CAUSE_MTIMER_S_TRAP:
      handle_mtimer_trap();
      break;
    case CAUSE_STORE_PAGE_FAULT:
    case CAUSE_LOAD_PAGE_FAULT:
      handle_user_page_fault(cause, read_csr(sepc), read_csr(stval));
      break;
    default:
      sprint("smode_trap_handler(): unexpected scause %p\n", cause);
      sprint("            sepc=%p stval=%p\n", read_csr(sepc), read_csr(stval));
      panic("unexpected exception happened.\n");
      break;
  }

  switch_to(p);
}
