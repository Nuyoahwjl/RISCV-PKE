/*
 * This app starts a very simple shell and executes commands listed in
 * hostfs_root/shellrc, one command per line.
 *
 * Line format:
 *   <command> [optional_arg]
 */
#include "user_lib.h"
#include "string.h"
#include "util/types.h"

static char *skip_space(char *s) {
  while (s && (*s == ' ' || *s == '\t')) s++;
  return s;
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  printu("\n======== Shell Start ========\n\n");

  enum { MAXBUF = 2048 };
  char buf[MAXBUF];

  int fd = open("/shellrc", O_RDONLY);
  if (fd < 0) {
    printu("open /shellrc failed\n");
    exit(-1);
    return -1;
  }

  int n = read_u(fd, buf, MAXBUF - 1);
  close(fd);
  if (n < 0) {
    printu("read /shellrc failed\n");
    exit(-1);
    return -1;
  }
  buf[n] = '\0';

  char *command = naive_malloc();
  char *para = naive_malloc();

  char *line = strtok(buf, "\n");
  while (line) {
    line = skip_space(line);

    // skip empty lines and comment lines
    if (line[0] == '\0' || line[0] == '#') {
      line = strtok(NULL, "\n");
      continue;
    }

    char *arg = 0;
    char *sep = strchr(line, ' ');
    if (!sep) sep = strchr(line, '\t');

    if (sep) {
      *sep = '\0';
      arg = skip_space(sep + 1);
      if (arg[0] == '\0') arg = 0;
    }

    strcpy(command, line);
    if (arg)
      strcpy(para, arg);
    else
      para[0] = '\0';

    // END or END END both stop the shellrc execution
    if (strcmp(command, "END") == 0) {
      if (!arg || strcmp(para, "END") == 0) break;
    }

    if (arg)
      printu("Next command: %s %s\n\n", command, para);
    else
      printu("Next command: %s\n\n", command);

    printu("==========Command Start============\n\n");

    int pid = fork();
    if (pid == 0) {
      int ret = exec(command, arg ? para : 0);
      if (ret == -1) printu("exec failed!\n");
      exit(-1);
    } else {
      wait(pid);
      printu("==========Command End============\n\n");
    }

    line = strtok(NULL, "\n");
  }

  exit(0);
  return 0;
}
