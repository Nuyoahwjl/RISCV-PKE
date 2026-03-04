/*
 * Machine-mode C startup codes
 */

#include "util/types.h"
#include "kernel/riscv.h"
#include "kernel/config.h"
#include "kernel/sync_utils.h"
#include "spike_interface/spike_utils.h"

//
// global variables are placed in the .data section.
// stack0 is the privilege mode stack(s) of the proxy kernel on CPU(s)
// allocates 4KB stack space for each processor (hart)
//
__attribute__((aligned(16))) char stack0[4096 * NCPU];

// sstart() is the supervisor state entry point defined in kernel/kernel.c
extern void s_start();
// M-mode trap entry point, added @lab1_2
extern void mtrapvec();

// htif is defined in spike_interface/spike_htif.c, marks the availability of HTIF
extern uint64 htif;
// g_mem_size is defined in spike_interface/spike_memory.c, size of the emulated memory
extern uint64 g_mem_size;

// per-hart interrupt frame (M-mode)
riscv_regs g_itrframe[NCPU];

// HTIF and spike file interface should be initialized only once (by hart0).
static volatile int htif_ready = 0;

void init_dtb(uint64 dtb) {
  query_htif(dtb);
  if (htif) sprint("HTIF is available!\r\n");

  query_mem(dtb);
  sprint("(Emulated) memory size: %ld MB\n", g_mem_size >> 20);
}

static void delegate_traps() {
  if (!supports_extension('S')) {
    sprint("S mode is not supported.\n");
    return;
  }

  uintptr_t interrupts = MIP_SSIP | MIP_STIP | MIP_SEIP;
  uintptr_t exceptions = (1U << CAUSE_MISALIGNED_FETCH) | (1U << CAUSE_FETCH_PAGE_FAULT) |
                         (1U << CAUSE_BREAKPOINT) | (1U << CAUSE_LOAD_PAGE_FAULT) |
                         (1U << CAUSE_STORE_PAGE_FAULT) | (1U << CAUSE_USER_ECALL);

  write_csr(mideleg, interrupts);
  write_csr(medeleg, exceptions);
  assert(read_csr(mideleg) == interrupts);
  assert(read_csr(medeleg) == exceptions);
}

void timerinit(uintptr_t hartid) {
  *(uint64*)CLINT_MTIMECMP(hartid) = *(uint64*)CLINT_MTIME + TIMER_INTERVAL;
  write_csr(mie, read_csr(mie) | MIE_MTIE);
}

void m_start(uintptr_t hartid, uintptr_t dtb) {
  // HTIF + spike file interface init is a *global* resource, do it only once on hart0.
  if (hartid == 0) {
    spike_file_init();
    init_dtb(dtb);

    // now HTIF is ready, safe to print.
    sprint("In m_start, hartid:%d\n", hartid);

    // publish: HTIF is ready for other harts.
    asm volatile("fence rw, rw" ::: "memory");
    htif_ready = 1;
  } else {
    // wait until hart0 finishes HTIF init, then we can use sprint/spike_file_* safely.
    while (!htif_ready) {
      asm volatile("nop");
    }
    sprint("In m_start, hartid:%d\n", hartid);
  }

  // per-hart mscratch
  write_csr(mscratch, &g_itrframe[hartid]);

  write_csr(mstatus, ((read_csr(mstatus) & ~MSTATUS_MPP_MASK) | MSTATUS_MPP_S));
  write_csr(mepc, (uint64)s_start);

  write_csr(mtvec, (uint64)mtrapvec);

  write_csr(mstatus, read_csr(mstatus) | MSTATUS_MIE);

  delegate_traps();

  write_csr(sie, read_csr(sie) | SIE_SEIE | SIE_STIE | SIE_SSIE);

  timerinit(hartid);

  asm volatile("mret");
}
