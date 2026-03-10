#include "pmm.h"
#include "util/functions.h"
#include "riscv.h"
#include "config.h"
#include "util/string.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"

// _end is defined in kernel/kernel.lds, it marks the ending (virtual) address of PKE kernel
extern char _end[];
// g_mem_size is defined in spike_interface/spike_memory.c
extern uint64 g_mem_size;

static uint64 free_mem_start_addr;
static uint64 free_mem_end_addr;

// physical page reference counting (for COW)
#define NPAGE ((PHYS_TOP - DRAM_BASE) / PGSIZE)
static uint16 g_page_refcnt[NPAGE];

static inline int pa2refidx(uint64 pa) {
  if (pa < DRAM_BASE || pa >= PHYS_TOP) return -1;
  return (int)((pa - DRAM_BASE) / PGSIZE);
}

int page_ref_get(void *pa) {
  int idx = pa2refidx((uint64)pa);
  if (idx < 0) return 0;
  return g_page_refcnt[idx];
}

void page_ref_inc(void *pa) {
  int idx = pa2refidx((uint64)pa);
  if (idx < 0) return;
  g_page_refcnt[idx]++;
}

int page_ref_dec(void *pa) {
  int idx = pa2refidx((uint64)pa);
  if (idx < 0) return 0;
  assert(g_page_refcnt[idx] > 0);
  g_page_refcnt[idx]--;
  return g_page_refcnt[idx];
}

typedef struct node {
  struct node *next;
} list_node;

// g_free_mem_list is the head of the list of free physical memory pages
static list_node g_free_mem_list;

static void create_freepage_list(uint64 start, uint64 end) {
  g_free_mem_list.next = 0;
  for (uint64 p = ROUNDUP(start, PGSIZE); p + PGSIZE < end; p += PGSIZE)
    free_page((void *)p);
}

void free_page(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (uint64)pa < free_mem_start_addr ||
      (uint64)pa >= free_mem_end_addr)
    panic("free_page 0x%lx \n", pa);

  int idx = pa2refidx((uint64)pa);
  if (idx >= 0) {
    // For kernel-internal pages, refcount is usually 1 at free time.
    // We only reject freeing a page that is still shared (ref > 1).
    if (g_page_refcnt[idx] > 1)
      panic("free_page on shared page: pa=0x%lx ref=%d\n", pa,
            g_page_refcnt[idx]);
    g_page_refcnt[idx] = 0;
  }

  list_node *n = (list_node *)pa;
  n->next = g_free_mem_list.next;
  g_free_mem_list.next = n;
}

void *alloc_page(void) {
  list_node *n = g_free_mem_list.next;
  if (n) g_free_mem_list.next = n->next;

  if (n) {
    int idx = pa2refidx((uint64)n);
    if (idx >= 0) g_page_refcnt[idx] = 1;
  }

  return (void *)n;
}

void pmm_init() {
  uint64 g_kernel_start = KERN_BASE;
  uint64 g_kernel_end = (uint64)&_end;

  memset(g_page_refcnt, 0, sizeof(g_page_refcnt));

  uint64 pke_kernel_size = g_kernel_end - g_kernel_start;
  sprint("PKE kernel start 0x%lx, PKE kernel end: 0x%lx, PKE kernel size: 0x%lx .\n",
         g_kernel_start, g_kernel_end, pke_kernel_size);

  free_mem_start_addr = ROUNDUP(g_kernel_end, PGSIZE);

  g_mem_size = MIN(PKE_MAX_ALLOWABLE_RAM, g_mem_size);
  if (g_mem_size < pke_kernel_size)
    panic("Error when recomputing physical memory size (g_mem_size).\n");

  free_mem_end_addr = g_mem_size + DRAM_BASE;
  sprint("free physical memory address: [0x%lx, 0x%lx] \n", free_mem_start_addr,
         free_mem_end_addr - 1);

  sprint("kernel memory manager is initializing ...\n");
  create_freepage_list(free_mem_start_addr, free_mem_end_addr);
}

