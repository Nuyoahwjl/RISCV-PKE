/*
 * The supporting library for applications.
 */

#include "user_lib.h"
#include "util/types.h"
#include "util/snprintf.h"
#include "kernel/syscall.h"

uint64 do_user_call(uint64 sysnum, uint64 a1, uint64 a2, uint64 a3, uint64 a4,
                    uint64 a5, uint64 a6, uint64 a7) {
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

void* naive_malloc() {
  return (void*)do_user_call(SYS_user_allocate_page, 0, 0, 0, 0, 0, 0, 0);
}

void naive_free(void* va) {
  do_user_call(SYS_user_free_page, (uint64)va, 0, 0, 0, 0, 0, 0);
}

int fork() {
  return do_user_call(SYS_user_fork, 0, 0, 0, 0, 0, 0, 0);
}

void yield() {
  do_user_call(SYS_user_yield, 0, 0, 0, 0, 0, 0, 0);
}

// -----------------------------------------------------------------------------
// semaphores (@lab3_challenge2)
// -----------------------------------------------------------------------------
int sem_new(int init_val) {
  return do_user_call(SYS_user_sem_new, (uint64)init_val, 0, 0, 0, 0, 0, 0);
}

int sem_P(int sem) {
  return do_user_call(SYS_user_sem_P, (uint64)sem, 0, 0, 0, 0, 0, 0);
}

int sem_V(int sem) {
  return do_user_call(SYS_user_sem_V, (uint64)sem, 0, 0, 0, 0, 0, 0);
}

int sem_free(int sem) {
  return do_user_call(SYS_user_sem_free, (uint64)sem, 0, 0, 0, 0, 0, 0);
}
