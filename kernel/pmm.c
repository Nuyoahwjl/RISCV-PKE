#include "pmm.h"
#include "util/functions.h"
#include "riscv.h"
#include "config.h"
#include "util/string.h"
#include "memlayout.h"
#include "spike_interface/spike_utils.h"

extern char _end[];
extern uint64 g_mem_size;

static uint64 free_mem_start_addr;
static uint64 free_mem_end_addr;

int vm_alloc_stage[NCPU] = { 0 }; // 0 for kernel alloc, 1 for user alloc

typedef struct node {
  struct node *next;
} list_node;

static list_node g_free_mem_list;

// A simple spin lock to protect the global free-page list in multicore.
typedef struct {
  volatile uint32 locked;
} spinlock_t;

static spinlock_t pmm_lock = {0};

static inline void pmm_lock_acquire() {
  uint32 old;
  do {
    asm volatile("amoswap.w.aq %0, %2, (%1)" : "=r"(old) : "r"(&pmm_lock.locked), "r"(1) : "memory");
  } while (old != 0);
}

static inline void pmm_lock_release() {
  asm volatile("amoswap.w.rl x0, %1, (%0)" : : "r"(&pmm_lock.locked), "r"(0) : "memory");
}

static void create_freepage_list(uint64 start, uint64 end) {
  g_free_mem_list.next = 0;
  for (uint64 p = ROUNDUP(start, PGSIZE); p + PGSIZE < end; p += PGSIZE)
    free_page( (void *)p );
}

void free_page(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (uint64)pa < free_mem_start_addr || (uint64)pa >= free_mem_end_addr)
    panic("free_page 0x%lx \n", pa);

  pmm_lock_acquire();
  list_node *n = (list_node *)pa;
  n->next = g_free_mem_list.next;
  g_free_mem_list.next = n;
  pmm_lock_release();
}

void *alloc_page(void) {
  pmm_lock_acquire();
  list_node *n = g_free_mem_list.next;
  if (n) g_free_mem_list.next = n->next;
  pmm_lock_release();

  int hartid = (int)read_tp();
  if (n && vm_alloc_stage[hartid]) {
    sprint("hartid = %d: alloc page 0x%x\n", hartid, (uint64)n);
  }
  return (void *)n;
}

void pmm_init() {
  uint64 g_kernel_start = KERN_BASE;
  uint64 g_kernel_end = (uint64)&_end;

  uint64 pke_kernel_size = g_kernel_end - g_kernel_start;
  sprint("PKE kernel start 0x%lx, PKE kernel end: 0x%lx, PKE kernel size: 0x%lx .\n",
    g_kernel_start, g_kernel_end, pke_kernel_size);

  free_mem_start_addr = ROUNDUP(g_kernel_end , PGSIZE);

  g_mem_size = MIN(PKE_MAX_ALLOWABLE_RAM, g_mem_size);
  if( g_mem_size < pke_kernel_size )
    panic( "Error when recomputing physical memory size (g_mem_size).\n" );

  free_mem_end_addr = g_mem_size + DRAM_BASE;
  sprint("free physical memory address: [0x%lx, 0x%lx] \n", free_mem_start_addr,
    free_mem_end_addr - 1);

  sprint("kernel memory manager is initializing ...\n");
  create_freepage_list(free_mem_start_addr, free_mem_end_addr);
}
