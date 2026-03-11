#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printu("usage: ls <path>\n");
    exit(-1);
    return -1;
  }

  char *path = argv[1];
  int dir_fd = opendir_u(path);
  if (dir_fd < 0) {
    printu("ls failed: %s\n", path);
    exit(-1);
    return -1;
  }
  printu("---------- ls command -----------\n");
  printu("ls \"%s\":\n", path);
  printu("[name]               [inode_num]\n");
  struct dir dir;
  int width = 20;
  while(readdir_u(dir_fd, &dir) == 0) {
    // we do not have %ms :(
    char name[width + 1];
    memset(name, ' ', width + 1);
    name[width] = '\0';
    if (strlen(dir.name) < width) {
      strcpy(name, dir.name);
      name[strlen(dir.name)] = ' ';
      printu("%s %d\n", name, dir.inum);
    }
    else
      printu("%s %d\n", dir.name, dir.inum);
  }
  printu("------------------------------\n");
  closedir_u(dir_fd);

  exit(0);
  return 0;
}
