/*
 * Utility functions for trap handling in Supervisor mode.
 */

#include "riscv.h"
#include "process.h"
#include "strap.h"
#include "syscall.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "util/functions.h"
#include "util/string.h"

#include "spike_interface/spike_utils.h"

static void handle_syscall(trapframe *tf) {
  tf->epc += 4;
  tf->regs.a0 = do_syscall(tf->regs.a0, tf->regs.a1, tf->regs.a2, tf->regs.a3,
                           tf->regs.a4, tf->regs.a5, tf->regs.a6, tf->regs.a7);
}

static uint64 g_ticks = 0;

void handle_mtimer_trap() {
  sprint("Ticks %d\n", g_ticks);
  g_ticks += 1;
  write_csr(sip, 0);
}

void handle_user_page_fault(uint64 mcause, uint64 sepc, uint64 stval) {
  (void)sepc;
  sprint("handle_page_fault: %lx\n", stval);

  // Copy-on-Write handling for write faults.
  if (mcause == CAUSE_STORE_PAGE_FAULT) {
    uint64 va = ROUNDDOWN(stval, PGSIZE);
    pte_t *pte = page_walk(current->pagetable, va, 0);
    if (pte && (*pte & PTE_V) && (*pte & PTE_COW)) {
      uint64 old_pa = PTE2PA(*pte);
      uint64 flags = PTE_FLAGS(*pte);

      if (page_ref_get((void *)old_pa) > 1) {
        void *new_pa = alloc_page();
        assert(new_pa);
        memcpy(new_pa, (void *)old_pa, PGSIZE);
        page_ref_dec((void *)old_pa);

        flags &= ~PTE_COW;
        flags |= (PTE_W | PTE_D);
        *pte = PA2PTE((uint64)new_pa) | flags;
      } else {
        flags &= ~PTE_COW;
        flags |= (PTE_W | PTE_D);
        *pte = PA2PTE(old_pa) | flags;
      }

      flush_tlb();
      return;
    }
  }

  switch (mcause) {
    case CAUSE_STORE_PAGE_FAULT:
      // Only auto-grow downward user stack; reject invalid addresses.
      if (stval < current->trapframe->regs.sp - PGSIZE)
        panic("this address is not available!");

      map_pages(current->pagetable,
                ROUNDDOWN(stval, PGSIZE),
                PGSIZE,
                (uint64)alloc_page(),
                prot_to_type(PROT_READ | PROT_WRITE, 1));
      break;
    default:
      sprint("unknown page fault.\n");
      break;
  }
}

void rrsched() {
  current->tick_count++;
  if (current->tick_count >= TIME_SLICE_LEN) {
    current->tick_count = 0;
    insert_to_ready_queue(current);
    schedule();
  }
}

void smode_trap_handler(void) {
  if ((read_csr(sstatus) & SSTATUS_SPP) != 0) panic("usertrap: not from user mode");

  assert(current);
  current->trapframe->epc = read_csr(sepc);

  uint64 cause = read_csr(scause);

  switch (cause) {
    case CAUSE_USER_ECALL:
      handle_syscall(current->trapframe);
      break;
    case CAUSE_MTIMER_S_TRAP:
      handle_mtimer_trap();
      rrsched();
      break;
    case CAUSE_STORE_PAGE_FAULT:
    case CAUSE_LOAD_PAGE_FAULT:
      handle_user_page_fault(cause, read_csr(sepc), read_csr(stval));
      break;
    default:
      sprint("smode_trap_handler(): unexpected scause %p\n", read_csr(scause));
      sprint("            sepc=%p stval=%p\n", read_csr(sepc), read_csr(stval));
      panic("unexpected exception happened.\n");
      break;
  }

  switch_to(current);
}
