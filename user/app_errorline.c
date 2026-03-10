/*
 * Below is the given application for lab1_challenge2 (same as lab1_2).
 */

#include "user_lib.h"
#include "util/types.h"

int main(void) {
  printu("Going to hack the system by running privilege instructions.\n");
  asm volatile("csrw sscratch, 0");
  exit(0);
  return 0;
}
