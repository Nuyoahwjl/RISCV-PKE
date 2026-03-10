/*
 * Below is the given application for lab2_challenge2_singlepageheap.
 */

#include "user_lib.h"

typedef unsigned long long uint64;

char *strcpy_local(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++))
    ;
  return dest;
}

int main(void) {
  char str[20] = "hello, world!!!";
  char *m = (char *)better_malloc(100);
  char *p = (char *)better_malloc(50);

  if ((uint64)p - (uint64)m > 512) {
    printu("you need to manage the vm space precisely!\n");
    exit(-1);
    return 0;
  }

  better_free((void *)m);

  strcpy_local(p, str);
  printu("%s\n", p);

  char *n = (char *)better_malloc(50);
  if (m != n) {
    printu("your malloc is not complete.\n");
    exit(-1);
    return 0;
  }

  exit(0);
  return 0;
}
