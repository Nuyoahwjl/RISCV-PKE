#ifndef _PMM_H_
#define _PMM_H_

// Initialize phisical memeory manager
void pmm_init();
// Allocate a free phisical page
void* alloc_page();
// Free an allocated page
void free_page(void* pa);

// page reference counting (for COW)
void page_ref_inc(void *pa);
int page_ref_dec(void *pa);
int page_ref_get(void *pa);

#endif
