/*
 * The supporting library for applications.
 */

#include "user_lib.h"
#include "util/types.h"
#include "util/snprintf.h"
#include "kernel/syscall.h"

uint64 do_user_call(uint64 sysnum, uint64 a1, uint64 a2, uint64 a3, uint64 a4, uint64 a5, uint64 a6,
                 uint64 a7) {
  int ret;
  asm volatile(
      "ecall\n"
      "sw a0, %0"
      : "=m"(ret)
      :
      : "memory");
  return ret;
}

int printu(const char* s, ...) {
  va_list vl;
  va_start(vl, s);

  char out[256];
  int res = vsnprintf(out, sizeof(out), s, vl);
  va_end(vl);
  const char* buf = out;
  size_t n = res < sizeof(out) ? res : sizeof(out);

  return do_user_call(SYS_user_print, (uint64)buf, n, 0, 0, 0, 0, 0);
}

int exit(int code) {
  return do_user_call(SYS_user_exit, code, 0, 0, 0, 0, 0, 0);
}

// lib call to better_malloc
void* better_malloc(int n) {
  return (void*)do_user_call(SYS_user_better_malloc, n, 0, 0, 0, 0, 0, 0);
}

// lib call to better_free
void better_free(void* va) {
  do_user_call(SYS_user_better_free, (uint64)va, 0, 0, 0, 0, 0, 0);
}
