/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"
#include "pmm.h"
#include "vmm.h"
#include "riscv.h"
#include "config.h"
#include "spike_interface/spike_utils.h"

static inline int atomic_add_w(volatile int *addr, int inc) {
  int old;
  asm volatile("amoadd.w %0, %2, (%1)" : "=r"(old) : "r"(addr), "r"(inc) : "memory");
  return old + inc;
}

static volatile int exited_harts = 0;

ssize_t sys_user_print(const char* buf, size_t n) {
  (void)n;
  int hartid = (int)read_tp();
  process *p = current[hartid];
  assert(p);

  char *pa = (char*)user_va_to_pa((pagetable_t)p->pagetable, (void*)buf);
  sprint(pa);
  return 0;
}

ssize_t sys_user_exit(uint64 code) {
  int hartid = (int)read_tp();
  sprint("hartid = %d: User exit with code: %d.\n", hartid, (int)code);

  atomic_add_w(&exited_harts, 1);

  if (hartid == 0) {
    while (exited_harts < NCPU) {
      asm volatile("wfi");
    }
    sprint("hartid = %d: shutdown with code: %d.\n", hartid, (int)code);
    shutdown(code);
  }

  while (1) {
    asm volatile("wfi");
  }

  return 0;
}

uint64 sys_user_allocate_page() {
  int hartid = (int)read_tp();
  process *p = current[hartid];
  assert(p);

  void* pa = alloc_page();
  uint64 va = g_ufree_page[hartid];
  g_ufree_page[hartid] += PGSIZE;

  user_vm_map((pagetable_t)p->pagetable, va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ, 1));

  sprint("hartid = %d: vaddr 0x%x is mapped to paddr 0x%x\n", hartid, (int)va, (uint64)pa);
  return va;
}

uint64 sys_user_free_page(uint64 va) {
  int hartid = (int)read_tp();
  process *p = current[hartid];
  assert(p);

  user_vm_unmap((pagetable_t)p->pagetable, va, PGSIZE, 1);
  return 0;
}

long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    case SYS_user_allocate_page:
      return sys_user_allocate_page();
    case SYS_user_free_page:
      return sys_user_free_page(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
