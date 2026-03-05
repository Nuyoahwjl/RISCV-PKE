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
#include "sched.h"

#include "spike_interface/spike_utils.h"

// -----------------------------------------------------------------------------
// Semaphores (@lab3_challenge2)
// -----------------------------------------------------------------------------
// NOTE: keep this data structure small; otherwise, kernel size may exceed limit.
#define NSEM 16

typedef struct semaphore_t {
  int used;
  int value;
  process *wait_head;
  process *wait_tail;
} semaphore;

static semaphore sem_pool[NSEM];

static inline int sem_valid(int semid) {
  return (semid >= 0) && (semid < NSEM) && sem_pool[semid].used;
}

static inline void sem_enqueue(semaphore *sem, process *p) {
  p->queue_next = NULL;
  if (sem->wait_tail) {
    sem->wait_tail->queue_next = p;
    sem->wait_tail = p;
  } else {
    sem->wait_head = sem->wait_tail = p;
  }
}

static inline process *sem_dequeue(semaphore *sem) {
  process *p = sem->wait_head;
  if (!p) return NULL;
  sem->wait_head = p->queue_next;
  if (!sem->wait_head) sem->wait_tail = NULL;
  p->queue_next = NULL;
  return p;
}

// create a semaphore, return semid (>=0) on success, -1 on failure.
ssize_t sys_user_sem_new(int init_val) {
  for (int i = 0; i < NSEM; i++) {
    if (!sem_pool[i].used) {
      sem_pool[i].used = 1;
      sem_pool[i].value = init_val;
      sem_pool[i].wait_head = NULL;
      sem_pool[i].wait_tail = NULL;
      return i;
    }
  }
  return -1;
}

// P operation: decrement semaphore; block if value becomes negative.
ssize_t sys_user_sem_P(int semid) {
  if (!sem_valid(semid)) return -1;
  semaphore *sem = &sem_pool[semid];

  sem->value--;
  if (sem->value < 0) {
    // block current process on this semaphore.
    current->status = BLOCKED;
    sem_enqueue(sem, current);
    // switch to another ready process.
    schedule();
  }
  return 0;
}

// V operation: increment semaphore; wake one blocked process if any.
ssize_t sys_user_sem_V(int semid) {
  if (!sem_valid(semid)) return -1;
  semaphore *sem = &sem_pool[semid];

  sem->value++;
  if (sem->value <= 0) {
    process *p = sem_dequeue(sem);
    if (p) {
      insert_to_ready_queue(p);
    }
  }
  return 0;
}

// free a semaphore. For simplicity, we refuse to free a semaphore that still has waiters.
ssize_t sys_user_sem_free(int semid) {
  if (!sem_valid(semid)) return -1;
  semaphore *sem = &sem_pool[semid];

  if (sem->wait_head) return -1;
  sem->used = 0;
  sem->value = 0;
  return 0;
}

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  assert(current);
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)buf);
  sprint(pa);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  free_process(current);
  schedule();
  return 0;
}

uint64 sys_user_allocate_page() {
  void* pa = alloc_page();
  uint64 va;
  if (current->user_heap.free_pages_count > 0) {
    va = current->user_heap.free_pages_address[--current->user_heap.free_pages_count];
    assert(va < current->user_heap.heap_top);
  } else {
    va = current->user_heap.heap_top;
    current->user_heap.heap_top += PGSIZE;
    current->mapped_info[HEAP_SEGMENT].npages++;
  }
  user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ, 1));
  return va;
}

uint64 sys_user_free_page(uint64 va) {
  user_vm_unmap((pagetable_t)current->pagetable, va, PGSIZE, 1);
  current->user_heap.free_pages_address[current->user_heap.free_pages_count++] = va;
  return 0;
}

ssize_t sys_user_fork() {
  sprint("User call fork.\n");
  return do_fork(current);
}

ssize_t sys_user_yield() {
  current->status = READY;
  insert_to_ready_queue(current);
  schedule();
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
    case SYS_user_fork:
      return sys_user_fork();
    case SYS_user_yield:
      return sys_user_yield();

    // added @lab3_challenge2
    case SYS_user_sem_new:
      return sys_user_sem_new((int)a1);
    case SYS_user_sem_P:
      return sys_user_sem_P((int)a1);
    case SYS_user_sem_V:
      return sys_user_sem_V((int)a1);
    case SYS_user_sem_free:
      return sys_user_sem_free((int)a1);

    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
