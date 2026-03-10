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
#include "proc_file.h"
#include "memlayout.h"
#include "vfs.h"
#include "elf.h"

#include "spike_interface/spike_utils.h"

// -----------------------------------------------------------------------------
// path helpers for relative-path challenge
// -----------------------------------------------------------------------------
static void resolve_user_path(const char *user_path, char *out) {
  if (user_path == 0 || out == 0) return;
  memset(out, 0, MAX_PATH_LEN);

  if (user_path[0] == '/') {
    strcpy(out, user_path);
    return;
  }

  struct dentry *cwd = current->pfiles->cwd;
  const char *cwd_name = cwd ? cwd->name : "/";

  if (user_path[0] == '.' && user_path[1] == '/') {
    out[0] = '/';
    if (cwd_name && cwd_name[0] != '/' && cwd_name[0] != '\0') {
      strcat(out, cwd_name);
      strcat(out, "/");
    }
    strcat(out, user_path + 2);
    return;
  }

  if (user_path[0] == '.' && user_path[1] == '.' && user_path[2] == '/') {
    struct dentry *parent = (cwd && cwd->parent) ? cwd->parent : cwd;
    const char *pname = parent ? parent->name : "/";
    out[0] = '/';
    if (pname && pname[0] != '/' && pname[0] != '\0') {
      strcat(out, pname);
      strcat(out, "/");
    }
    strcat(out, user_path + 3);
    return;
  }

  // default: treat as cwd-relative plain name
  out[0] = '/';
  if (cwd_name && cwd_name[0] != '/' && cwd_name[0] != '\0') {
    strcat(out, cwd_name);
    strcat(out, "/");
  }
  strcat(out, user_path);
}

// -----------------------------------------------------------------------------
// backtrace challenge
// -----------------------------------------------------------------------------
static uint64 read_user_u64(uint64 uva) {
  uint64 pa = (uint64)user_va_to_pa((pagetable_t)(current->pagetable), (void *)uva);
  if (pa == 0) return 0;
  return *((uint64 *)pa);
}

static ssize_t find_func_name(uint64 ra) {
  for (int i = 0; i < sym_count; i++) {
    if (ra >= symbols[i].st_value && ra < symbols[i].st_value + symbols[i].st_size) {
      sprint("%s\n", sym_names[i]);
      if (strcmp(sym_names[i], "main") == 0) return 1;
      return 0;
    }
  }
  return 0;
}

ssize_t sys_user_print_backtrace(uint64 n) {
  uint64 fp = current->trapframe->regs.s0;
  if (fp == 0) return 0;

  for (uint64 i = 0; i < n; i++) {
    if (fp < 16) break;
    uint64 ra = read_user_u64(fp - 8);
    if (find_func_name(ra)) return 0;

    uint64 prev_fp = read_user_u64(fp - 16);
    if (prev_fp == 0 || prev_fp == fp) break;
    fp = prev_fp;
  }
  return 0;
}

// -----------------------------------------------------------------------------
// better_malloc / better_free challenge
// -----------------------------------------------------------------------------
#define HEAP_ALIGNMENT 16ULL
#define HEAP_MIN_SPLIT 16ULL

typedef struct heap_blk_t {
  uint64 size;
  uint64 next;
  uint64 prev;
  uint64 free;
} heap_blk_t;

static inline uint64 heap_align_up(uint64 x) {
  return (x + HEAP_ALIGNMENT - 1) & ~(HEAP_ALIGNMENT - 1);
}

static inline void *uva2kva(process *proc, uint64 uva) {
  uint64 pa_base = lookup_pa((pagetable_t)proc->pagetable, uva);
  if (pa_base == 0) return 0;
  return (void *)(pa_base + (uva & (PGSIZE - 1)));
}

static inline heap_blk_t *heap_hdr(process *proc, uint64 hdr_uva) {
  return (heap_blk_t *)uva2kva(proc, hdr_uva);
}

static void heap_map_one_page(process *proc, uint64 va) {
  void *pa = alloc_page();
  if (!pa) panic("out of physical memory in heap_map_one_page\n");
  user_vm_map((pagetable_t)proc->pagetable, va, PGSIZE, (uint64)pa,
              prot_to_type(PROT_WRITE | PROT_READ, 1));
}

static void heap_extend(process *proc, uint64 min_bytes) {
  assert(proc);
  uint64 pages = (min_bytes + PGSIZE - 1) / PGSIZE;
  if (pages == 0) pages = 1;

  uint64 old_end = proc->heap_end;
  for (uint64 i = 0; i < pages; i++) {
    heap_map_one_page(proc, proc->heap_end);
    proc->heap_end += PGSIZE;
  }

  if (proc->heap_head == 0) {
    uint64 bva = USER_FREE_ADDRESS_START;
    heap_blk_t *b = heap_hdr(proc, bva);
    assert(b);
    b->size = pages * PGSIZE - sizeof(heap_blk_t);
    b->next = 0;
    b->prev = 0;
    b->free = 1;
    proc->heap_head = bva;
    proc->heap_tail = bva;
    return;
  }

  heap_blk_t *tail = heap_hdr(proc, proc->heap_tail);
  assert(tail);
  uint64 tail_end = proc->heap_tail + sizeof(heap_blk_t) + tail->size;
  if (tail->free && tail_end == old_end) {
    tail->size += pages * PGSIZE;
    return;
  }

  uint64 nbva = old_end;
  heap_blk_t *nb = heap_hdr(proc, nbva);
  assert(nb);
  nb->size = pages * PGSIZE - sizeof(heap_blk_t);
  nb->free = 1;
  nb->prev = proc->heap_tail;
  nb->next = 0;
  tail->next = nbva;
  proc->heap_tail = nbva;
}

static uint64 heap_alloc_from(process *proc, uint64 blk_hdr_uva, uint64 need) {
  heap_blk_t *b = heap_hdr(proc, blk_hdr_uva);
  assert(b && b->free && b->size >= need);

  uint64 remain = b->size - need;
  if (remain >= sizeof(heap_blk_t) + HEAP_MIN_SPLIT) {
    uint64 nhdr_uva = blk_hdr_uva + sizeof(heap_blk_t) + need;
    heap_blk_t *nb = heap_hdr(proc, nhdr_uva);
    assert(nb);

    nb->size = remain - sizeof(heap_blk_t);
    nb->free = 1;
    nb->prev = blk_hdr_uva;
    nb->next = b->next;

    if (b->next) {
      heap_blk_t *nxt = heap_hdr(proc, b->next);
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

static uint64 heap_alloc(process *proc, uint64 nbytes) {
  if (nbytes == 0) return 0;
  uint64 need = heap_align_up(nbytes);

  if (proc->heap_head == 0) {
    heap_extend(proc, need + sizeof(heap_blk_t));
  }

  uint64 cur_uva = proc->heap_head;
  while (cur_uva) {
    heap_blk_t *cur = heap_hdr(proc, cur_uva);
    assert(cur);
    if (cur->free && cur->size >= need) {
      return heap_alloc_from(proc, cur_uva, need);
    }
    cur_uva = cur->next;
  }

  heap_extend(proc, need + sizeof(heap_blk_t));
  heap_blk_t *tail = heap_hdr(proc, proc->heap_tail);
  assert(tail && tail->free && tail->size >= need);
  return heap_alloc_from(proc, proc->heap_tail, need);
}

static void heap_free(process *proc, uint64 user_ptr) {
  if (user_ptr == 0) return;
  if (user_ptr < USER_FREE_ADDRESS_START + sizeof(heap_blk_t)) return;

  uint64 hdr_uva = user_ptr - sizeof(heap_blk_t);
  heap_blk_t *b = heap_hdr(proc, hdr_uva);
  if (!b) return;
  b->free = 1;

  if (b->next) {
    heap_blk_t *n = heap_hdr(proc, b->next);
    if (n && n->free && (hdr_uva + sizeof(heap_blk_t) + b->size == b->next)) {
      b->size += sizeof(heap_blk_t) + n->size;
      b->next = n->next;
      if (n->next) {
        heap_blk_t *nn = heap_hdr(proc, n->next);
        assert(nn);
        nn->prev = hdr_uva;
      } else {
        proc->heap_tail = hdr_uva;
      }
    }
  }

  if (b->prev) {
    uint64 phdr_uva = b->prev;
    heap_blk_t *p = heap_hdr(proc, phdr_uva);
    if (p && p->free && (phdr_uva + sizeof(heap_blk_t) + p->size == hdr_uva)) {
      p->size += sizeof(heap_blk_t) + b->size;
      p->next = b->next;
      if (b->next) {
        heap_blk_t *nn = heap_hdr(proc, b->next);
        assert(nn);
        nn->prev = phdr_uva;
      } else {
        proc->heap_tail = phdr_uva;
      }
    }
  }
}

uint64 sys_user_better_malloc(uint64 nbytes) {
  assert(current);
  return heap_alloc(current, nbytes);
}

uint64 sys_user_better_free(uint64 user_ptr) {
  assert(current);
  heap_free(current, user_ptr);
  return 0;
}

// -----------------------------------------------------------------------------
// semaphores challenge
// -----------------------------------------------------------------------------
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

ssize_t sys_user_sem_P(int semid) {
  if (!sem_valid(semid)) return -1;
  semaphore *sem = &sem_pool[semid];

  sem->value--;
  if (sem->value < 0) {
    current->status = BLOCKED;
    sem_enqueue(sem, current);
    schedule();
  }
  return 0;
}

ssize_t sys_user_sem_V(int semid) {
  if (!sem_valid(semid)) return -1;
  semaphore *sem = &sem_pool[semid];

  sem->value++;
  if (sem->value <= 0) {
    process *p = sem_dequeue(sem);
    if (p) insert_to_ready_queue(p);
  }
  return 0;
}

ssize_t sys_user_sem_free(int semid) {
  if (!sem_valid(semid)) return -1;
  semaphore *sem = &sem_pool[semid];
  if (sem->wait_head) return -1;
  sem->used = 0;
  sem->value = 0;
  return 0;
}

// -----------------------------------------------------------------------------
// base syscalls
// -----------------------------------------------------------------------------
ssize_t sys_user_print(const char *buf, size_t n) {
  (void)n;
  assert(current);
  char *pa = (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)buf);
  sprint(pa);
  return 0;
}

ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  free_process(current);
  remove_block_and_insert(current);
  schedule();
  return 0;
}

uint64 sys_user_allocate_page() {
  void *pa = alloc_page();
  uint64 va;

  if (current->user_heap.free_pages_count > 0) {
    va = current->user_heap
             .free_pages_address[--current->user_heap.free_pages_count];
    assert(va < current->user_heap.heap_top);
  } else {
    va = current->user_heap.heap_top;
    current->user_heap.heap_top += PGSIZE;
    current->mapped_info[HEAP_SEGMENT].npages++;
  }

  user_vm_map((pagetable_t)current->pagetable,
              va,
              PGSIZE,
              (uint64)pa,
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

ssize_t sys_user_wait(long pid) { return do_wait(pid); }

ssize_t sys_user_exec(char *pathva, char *argva) {
  char *pathpa =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)pathva);

  char *argpa = 0;
  if (argva)
    argpa =
        (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)argva);

  return do_exec(current, pathpa, argpa);
}

ssize_t sys_user_printpa(uint64 va) {
  uint64 pa =
      (uint64)user_va_to_pa((pagetable_t)(current->pagetable), (void *)va);
  sprint("%lx\n", pa);
  return 0;
}

// -----------------------------------------------------------------------------
// file/directory syscalls
// -----------------------------------------------------------------------------
ssize_t sys_user_open(char *pathva, int flags) {
  char *pathpa =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)pathva);
  char kpath[MAX_PATH_LEN];
  resolve_user_path(pathpa, kpath);
  return do_open(kpath, flags);
}

ssize_t sys_user_read(int fd, char *bufva, uint64 count) {
  int i = 0;
  while (i < count) {
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_read(fd, (char *)pa + off, len);
    i += r;
    if (r < len) return i;
  }
  return count;
}

ssize_t sys_user_write(int fd, char *bufva, uint64 count) {
  int i = 0;
  while (i < count) {
    uint64 addr = (uint64)bufva + i;
    uint64 pa = lookup_pa((pagetable_t)current->pagetable, addr);
    uint64 off = addr - ROUNDDOWN(addr, PGSIZE);
    uint64 len = count - i < PGSIZE - off ? count - i : PGSIZE - off;
    uint64 r = do_write(fd, (char *)pa + off, len);
    i += r;
    if (r < len) return i;
  }
  return count;
}

ssize_t sys_user_lseek(int fd, int offset, int whence) {
  return do_lseek(fd, offset, whence);
}

ssize_t sys_user_stat(int fd, struct istat *istat) {
  struct istat *pistat =
      (struct istat *)user_va_to_pa((pagetable_t)(current->pagetable), istat);
  return do_stat(fd, pistat);
}

ssize_t sys_user_disk_stat(int fd, struct istat *istat) {
  struct istat *pistat =
      (struct istat *)user_va_to_pa((pagetable_t)(current->pagetable), istat);
  return do_disk_stat(fd, pistat);
}

ssize_t sys_user_close(int fd) { return do_close(fd); }

ssize_t sys_user_opendir(char *pathva) {
  char *pathpa =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)pathva);
  char kpath[MAX_PATH_LEN];
  resolve_user_path(pathpa, kpath);
  return do_opendir(kpath);
}

ssize_t sys_user_readdir(int fd, struct dir *vdir) {
  struct dir *pdir =
      (struct dir *)user_va_to_pa((pagetable_t)(current->pagetable), vdir);
  return do_readdir(fd, pdir);
}

ssize_t sys_user_mkdir(char *pathva) {
  char *pathpa =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)pathva);
  char kpath[MAX_PATH_LEN];
  resolve_user_path(pathpa, kpath);
  return do_mkdir(kpath);
}

ssize_t sys_user_closedir(int fd) { return do_closedir(fd); }

ssize_t sys_user_link(char *vfn1, char *vfn2) {
  char *pfn1 =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)vfn1);
  char *pfn2 =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)vfn2);

  char kfn1[MAX_PATH_LEN];
  char kfn2[MAX_PATH_LEN];
  resolve_user_path(pfn1, kfn1);
  resolve_user_path(pfn2, kfn2);

  return do_link(kfn1, kfn2);
}

ssize_t sys_user_unlink(char *vfn) {
  char *pfn =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)vfn);
  char kfn[MAX_PATH_LEN];
  resolve_user_path(pfn, kfn);
  return do_unlink(kfn);
}

ssize_t sys_user_rcwd(char *pathva) {
  char path[MAX_PATH_LEN];
  memset(path, 0, sizeof(path));

  char *cwd_name = current->pfiles->cwd ? current->pfiles->cwd->name : "/";
  if (cwd_name[0] != '/') {
    path[0] = '/';
    strcpy(path + 1, cwd_name);
  } else {
    strcpy(path, cwd_name);
  }

  char *dst =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)pathva);
  strcpy(dst, path);
  return 0;
}

ssize_t sys_user_ccwd(char *pathva) {
  char *user_pa =
      (char *)user_va_to_pa((pagetable_t)(current->pagetable), (void *)pathva);

  struct dentry *cwd = current->pfiles->cwd;
  struct dentry *target = cwd;

  if (strcmp(user_pa, ".") == 0) return 0;

  if (user_pa[0] == '.' && user_pa[1] == '/') {
    target = hash_get_dentry(cwd, user_pa + 2);
  } else if (strcmp(user_pa, "..") == 0) {
    target = cwd && cwd->parent ? cwd->parent : cwd;
  } else if (user_pa[0] == '.' && user_pa[1] == '.' && user_pa[2] == '/') {
    struct dentry *base = cwd && cwd->parent ? cwd->parent : cwd;
    target = hash_get_dentry(base, user_pa + 3);
  } else {
    const char *name = user_pa[0] == '/' ? user_pa + 1 : user_pa;
    target = hash_get_dentry(vfs_root_dentry, (char *)name);
  }

  if (target == 0 || target->dentry_inode == 0 || target->dentry_inode->type != DIR_I)
    return -1;

  current->pfiles->cwd = target;
  return 0;
}

long do_syscall(long a0,
                long a1,
                long a2,
                long a3,
                long a4,
                long a5,
                long a6,
                long a7) {
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  (void)a7;

  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char *)a1, a2);
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
    case SYS_user_wait:
      return sys_user_wait(a1);

    case SYS_user_print_backtrace:
      return sys_user_print_backtrace(a1);
    case SYS_user_better_malloc:
      return sys_user_better_malloc(a1);
    case SYS_user_better_free:
      return sys_user_better_free(a1);
    case SYS_user_sem_new:
      return sys_user_sem_new((int)a1);
    case SYS_user_sem_P:
      return sys_user_sem_P((int)a1);
    case SYS_user_sem_V:
      return sys_user_sem_V((int)a1);
    case SYS_user_sem_free:
      return sys_user_sem_free((int)a1);
    case SYS_user_printpa:
      return sys_user_printpa(a1);
    case SYS_user_rcwd:
      return sys_user_rcwd((char *)a1);
    case SYS_user_ccwd:
      return sys_user_ccwd((char *)a1);

    case SYS_user_open:
      return sys_user_open((char *)a1, a2);
    case SYS_user_read:
      return sys_user_read(a1, (char *)a2, a3);
    case SYS_user_write:
      return sys_user_write(a1, (char *)a2, a3);
    case SYS_user_lseek:
      return sys_user_lseek(a1, a2, a3);
    case SYS_user_stat:
      return sys_user_stat(a1, (struct istat *)a2);
    case SYS_user_disk_stat:
      return sys_user_disk_stat(a1, (struct istat *)a2);
    case SYS_user_close:
      return sys_user_close(a1);
    case SYS_user_opendir:
      return sys_user_opendir((char *)a1);
    case SYS_user_readdir:
      return sys_user_readdir(a1, (struct dir *)a2);
    case SYS_user_mkdir:
      return sys_user_mkdir((char *)a1);
    case SYS_user_closedir:
      return sys_user_closedir(a1);
    case SYS_user_link:
      return sys_user_link((char *)a1, (char *)a2);
    case SYS_user_unlink:
      return sys_user_unlink((char *)a1);
    case SYS_user_exec:
      return sys_user_exec((char *)a1, (char *)a2);

    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
