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
#include "riscv.h"
#include "config.h"

#include "spike_interface/spike_utils.h"

// atomic add (word) using AMO, safe across multiple harts.
static inline int atomic_add_w(volatile int *addr, int inc) {
  int old;
  asm volatile("amoadd.w %0, %2, (%1)\n" : "=r"(old) : "r"(addr), "r"(inc) : "memory");
  return old + inc;
}

// number of harts that have called SYS_user_exit.
static volatile int exited_harts = 0;

ssize_t sys_user_print(const char* buf, size_t n) {
  (void)n;
  int hartid = (int)read_tp();
  // Do NOT append extra '\n' here; user string usually already contains it.
  sprint("hartid = %d: %s", hartid, buf);
  return 0;
}

ssize_t sys_user_exit(uint64 code) {
  int hartid = (int)read_tp();
  sprint("hartid = %d: User exit with code:%d.\n", hartid, code);

  atomic_add_w(&exited_harts, 1);

  if (hartid == 0) {
    while (exited_harts < NCPU) {
      asm volatile("wfi");
    }
    sprint("hartid = %d: shutdown with code:%d.\n", hartid, code);
    shutdown(code);
  }

  while (1) {
    asm volatile("wfi");
  }

  return 0;
}

long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
