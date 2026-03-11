#include "user_lib.h"
#include "util/types.h"

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  printu("\n======== exec /bin/app_ls in app_exec ========\n");
  char *exec_argv[] = {"/bin/app_ls", "/RAMDISK0"};
  int ret = exec("/bin/app_ls", 2, exec_argv);
  if (ret == -1)
    printu("exec failed!\n");

  exit(0);
  return 0;
}
