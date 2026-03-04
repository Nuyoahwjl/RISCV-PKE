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
#include "memlayout.h"
#include "spike_interface/spike_utils.h"

// --- a tiny heap allocator for lab2_challenge2 ---
// The allocator manages the user heap as a doubly-linked list of blocks.
// Metadata (block headers) are stored *inside* the user heap itself.

#define HEAP_ALIGNMENT 16ULL
#define HEAP_MIN_SPLIT 16ULL

typedef struct heap_blk_t {
  // payload size in bytes (NOT including header)
  uint64 size;
  // virtual address of next/prev block header in user heap (0 means NULL)
  uint64 next;
  uint64 prev;
  // 1: free, 0: allocated
  uint64 free;
} heap_blk_t;

static inline uint64 heap_align_up(uint64 x) {
  return (x + HEAP_ALIGNMENT - 1) & ~(HEAP_ALIGNMENT - 1);
}

// translate a user virtual address to a kernel-usable pointer.
// kernel maps physical memory with direct mapping, so pa can be dereferenced directly.
static inline void* uva2kva(process* proc, uint64 uva) {
  uint64 pa_base = lookup_pa((pagetable_t)proc->pagetable, uva);
  if (pa_base == 0) return 0;
  return (void*)(pa_base + (uva & (PGSIZE - 1)));
}

static inline heap_blk_t* heap_hdr(process* proc, uint64 hdr_uva) {
  return (heap_blk_t*)uva2kva(proc, hdr_uva);
}

static void heap_map_one_page(process* proc, uint64 va) {
  void* pa = alloc_page();
  if (!pa) panic("out of physical memory in heap_map_one_page\n");
  user_vm_map((pagetable_t)proc->pagetable, va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ, 1));
}

// extend heap by at least 'min_bytes' bytes of *total* space (header+payload).
// it always grows by pages, starting at proc->heap_end.
static void heap_extend(process* proc, uint64 min_bytes) {
  assert(proc);
  uint64 pages = (min_bytes + PGSIZE - 1) / PGSIZE;
  if (pages == 0) pages = 1;

  uint64 old_end = proc->heap_end;
  for (uint64 i = 0; i < pages; i++) {
    heap_map_one_page(proc, proc->heap_end);
    proc->heap_end += PGSIZE;
  }

  // first-time initialization: create one big free block.
  if (proc->heap_head == 0) {
    uint64 bva = USER_FREE_ADDRESS_START;
    heap_blk_t* b = heap_hdr(proc, bva);
    assert(b);
    b->size = pages * PGSIZE - sizeof(heap_blk_t);
    b->next = 0;
    b->prev = 0;
    b->free = 1;
    proc->heap_head = bva;
    proc->heap_tail = bva;
    return;
  }

  // otherwise, try to merge the newly added pages into the tail block if it is free
  // and ends exactly at old_end.
  heap_blk_t* tail = heap_hdr(proc, proc->heap_tail);
  assert(tail);
  uint64 tail_end = proc->heap_tail + sizeof(heap_blk_t) + tail->size;
  if (tail->free && tail_end == old_end) {
    tail->size += pages * PGSIZE;
    return;
  }

  // create a new free block at old_end.
  uint64 nbva = old_end;
  heap_blk_t* nb = heap_hdr(proc, nbva);
  assert(nb);
  nb->size = pages * PGSIZE - sizeof(heap_blk_t);
  nb->free = 1;
  nb->prev = proc->heap_tail;
  nb->next = 0;
  tail->next = nbva;
  proc->heap_tail = nbva;
}

static uint64 heap_alloc_from(process* proc, uint64 blk_hdr_uva, uint64 need) {
  heap_blk_t* b = heap_hdr(proc, blk_hdr_uva);
  assert(b && b->free && b->size >= need);

  uint64 remain = b->size - need;
  if (remain >= sizeof(heap_blk_t) + HEAP_MIN_SPLIT) {
    uint64 nhdr_uva = blk_hdr_uva + sizeof(heap_blk_t) + need;
    heap_blk_t* nb = heap_hdr(proc, nhdr_uva);
    assert(nb);

    nb->size = remain - sizeof(heap_blk_t);
    nb->free = 1;
    nb->prev = blk_hdr_uva;
    nb->next = b->next;

    if (b->next) {
      heap_blk_t* nxt = heap_hdr(proc, b->next);
      assert(nxt);
      nxt->prev = nhdr_uva;
    } else {
      proc->heap_tail = nhdr_uva;
    }

    b->next = nhdr_uva;
    b->size = need;
  }

  b->free = 0;
  return blk_hdr_uva + sizeof(heap_blk_t);
}

static uint64 heap_alloc(process* proc, uint64 nbytes) {
  if (nbytes == 0) return 0;
  uint64 need = heap_align_up(nbytes);

  // ensure heap has at least one mapped page.
  if (proc->heap_head == 0) {
    heap_extend(proc, need + sizeof(heap_blk_t));
  }

  // first-fit search.
  uint64 cur_uva = proc->heap_head;
  while (cur_uva) {
    heap_blk_t* cur = heap_hdr(proc, cur_uva);
    assert(cur);
    if (cur->free && cur->size >= need) {
      return heap_alloc_from(proc, cur_uva, need);
    }
    cur_uva = cur->next;
  }

  // no fit: extend heap and allocate from the (new/expanded) tail block.
  heap_extend(proc, need + sizeof(heap_blk_t));
  heap_blk_t* tail = heap_hdr(proc, proc->heap_tail);
  assert(tail && tail->free && tail->size >= need);
  return heap_alloc_from(proc, proc->heap_tail, need);
}

static void heap_free(process* proc, uint64 user_ptr) {
  if (user_ptr == 0) return;
  if (user_ptr < USER_FREE_ADDRESS_START + sizeof(heap_blk_t)) return;

  uint64 hdr_uva = user_ptr - sizeof(heap_blk_t);
  heap_blk_t* b = heap_hdr(proc, hdr_uva);
  if (!b) return;
  b->free = 1;

  // coalesce with next
  if (b->next) {
    heap_blk_t* n = heap_hdr(proc, b->next);
    if (n && n->free && (hdr_uva + sizeof(heap_blk_t) + b->size == b->next)) {
      b->size += sizeof(heap_blk_t) + n->size;
      b->next = n->next;
      if (n->next) {
        heap_blk_t* nn = heap_hdr(proc, n->next);
        assert(nn);
        nn->prev = hdr_uva;
      } else {
        proc->heap_tail = hdr_uva;
      }
    }
  }

  // coalesce with prev
  if (b->prev) {
    uint64 phdr_uva = b->prev;
    heap_blk_t* p = heap_hdr(proc, phdr_uva);
    if (p && p->free && (phdr_uva + sizeof(heap_blk_t) + p->size == hdr_uva)) {
      p->size += sizeof(heap_blk_t) + b->size;
      p->next = b->next;
      if (b->next) {
        heap_blk_t* nn = heap_hdr(proc, b->next);
        assert(nn);
        nn->prev = phdr_uva;
      } else {
        proc->heap_tail = phdr_uva;
      }
    }
  }
}

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  // buf is now an address in user space of the given app's user stack,
  // so we have to transfer it into phisical address (kernel is running in direct mapping).
  assert( current );
  char* pa = (char*)user_va_to_pa((pagetable_t)(current->pagetable), (void*)buf);
  sprint(pa);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  // in lab1, PKE considers only one app (one process). 
  // therefore, shutdown the system when the app calls exit()
  shutdown(code);
}

//
// maybe, the simplest implementation of malloc in the world ... added @lab2_2
//
uint64 sys_user_allocate_page() {
  void* pa = alloc_page();
  uint64 va = g_ufree_page;
  g_ufree_page += PGSIZE;
  user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
         prot_to_type(PROT_WRITE | PROT_READ, 1));

  return va;
}

//
// reclaim a page, indicated by "va". added @lab2_2
//
uint64 sys_user_free_page(uint64 va) {
  user_vm_unmap((pagetable_t)current->pagetable, va, PGSIZE, 1);
  return 0;
}

//
// better_malloc/better_free for lab2_challenge2
//
uint64 sys_user_better_malloc(uint64 nbytes) {
  assert(current);
  return heap_alloc(current, nbytes);
}

uint64 sys_user_better_free(uint64 user_ptr) {
  assert(current);
  heap_free(current, user_ptr);
  return 0;
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    // added @lab2_2
    case SYS_user_allocate_page:
      return sys_user_allocate_page();
    case SYS_user_free_page:
      return sys_user_free_page(a1);
    case SYS_user_better_malloc:
      return sys_user_better_malloc(a1);
    case SYS_user_better_free:
      return sys_user_better_free(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}

