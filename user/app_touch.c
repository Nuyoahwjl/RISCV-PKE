#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printu("usage: touch <file>\n");
    exit(-1);
    return -1;
  }

  int fd;
  char *filename = argv[1];
  printu("\n======== touch command ========\n");
  printu("touch: %s\n", filename);

  fd = open(filename, O_CREAT);
  printu("file descriptor fd: %d\n", fd);

  close(fd);
  exit(0);
  return 0;
}
