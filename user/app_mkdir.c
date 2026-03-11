#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"


int main(int argc, char *argv[]) {
  if (argc < 2) {
    printu("usage: mkdir <dir>\n");
    exit(-1);
    return -1;
  }

  char *new_dir = argv[1];
  printu("\n======== mkdir command ========\n");

  mkdir_u(new_dir);
  printu("mkdir: %s\n", new_dir);

  exit(0);
  return 0;
}
