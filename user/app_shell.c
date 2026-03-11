#include "user_lib.h"
#include "util/string.h"
#include "util/types.h"

#define SHELL_MAX_LINE 128
#define SHELL_MAX_ARGS 8
#define SHELL_MAX_HISTORY 16
#define SHELL_MAX_JOBS 16
#define SHELL_MAX_SCRIPT 2048

typedef struct shell_job_t {
  int active;
  int pid;
  char command[SHELL_MAX_LINE];
} shell_job;

typedef struct command_alias_t {
  const char *name;
  const char *path;
} command_alias;

static shell_job g_jobs[SHELL_MAX_JOBS];
static char g_history[SHELL_MAX_HISTORY][SHELL_MAX_LINE];
static int g_history_total = 0;

static const command_alias g_aliases[] = {
    {"ls", "/bin/app_ls"},
    {"mkdir", "/bin/app_mkdir"},
    {"touch", "/bin/app_touch"},
    {"cat", "/bin/app_cat"},
    {"writehello", "/bin/app_echo"},
    {"backtrace", "/bin/app_print_backtrace"},
    {"errorline", "/bin/app_errorline"},
    {"heap", "/bin/app_singlepageheap"},
    {"alloc0", "/bin/app_alloc0"},
    {"alloc1", "/bin/app_alloc1"},
    {"waitdemo", "/bin/app_wait"},
    {"sem", "/bin/app_semaphore"},
    {"cow", "/bin/app_cow"},
    {"relative", "/bin/app_relativepath"},
    {"execdemo", "/bin/app_exec"},
    {"app0", "/bin/app0"},
    {"app1", "/bin/app1"},
    {"sum", "/bin/app_sum_sequence"},
    {0, 0},
};

enum command_result {
  COMMAND_OK = 0,
  COMMAND_EXIT = 1,
};

static int is_space(char ch) {
  return ch == ' ' || ch == '\t';
}

static void print_prompt(void) {
  char cwd[32];
  read_cwd(cwd);
  printu("[pke:%s]$ ", cwd);
}

static void add_history(const char *line) {
  if (!line || line[0] == '\0') return;
  safestrcpy(g_history[g_history_total % SHELL_MAX_HISTORY], line, SHELL_MAX_LINE);
  g_history_total++;
}

static void print_history(void) {
  int start = g_history_total > SHELL_MAX_HISTORY ? g_history_total - SHELL_MAX_HISTORY : 0;
  for (int i = start; i < g_history_total; i++) {
    printu("%d %s\n", i + 1, g_history[i % SHELL_MAX_HISTORY]);
  }
}

static int find_job_slot_by_pid(int pid) {
  for (int i = 0; i < SHELL_MAX_JOBS; i++)
    if (g_jobs[i].active && g_jobs[i].pid == pid) return i;
  return -1;
}

static int alloc_job_slot(void) {
  for (int i = 0; i < SHELL_MAX_JOBS; i++)
    if (!g_jobs[i].active) return i;
  return -1;
}

static void finish_job(int pid, int announce) {
  int slot = find_job_slot_by_pid(pid);
  if (slot < 0) return;

  if (announce)
    printu("[done] %d %s\n", g_jobs[slot].pid, g_jobs[slot].command);

  g_jobs[slot].active = 0;
  g_jobs[slot].pid = 0;
  g_jobs[slot].command[0] = '\0';
}

static void add_job(int pid, const char *command) {
  int slot = alloc_job_slot();
  if (slot < 0) {
    printu("job table full, wait foreground pid %d\n", pid);
    waitpid(pid, 0);
    return;
  }

  g_jobs[slot].active = 1;
  g_jobs[slot].pid = pid;
  safestrcpy(g_jobs[slot].command, command, SHELL_MAX_LINE);
  printu("[bg] %d %s\n", pid, g_jobs[slot].command);
}

static int active_job_count(void) {
  int count = 0;
  for (int i = 0; i < SHELL_MAX_JOBS; i++)
    if (g_jobs[i].active) count++;
  return count;
}

static void print_jobs(void) {
  for (int i = 0; i < SHELL_MAX_JOBS; i++) {
    if (g_jobs[i].active) printu("[%d] running %s\n", g_jobs[i].pid, g_jobs[i].command);
  }
}

static void reap_background_jobs(void) {
  while (1) {
    int pid = waitpid(-1, 1);
    if (pid <= 0) break;
    finish_job(pid, 1);
  }
}

static int read_line(char *buf, int maxlen) {
  int len = 0;

  while (1) {
    int ch = getchar_u();

    if (ch == '\r' || ch == '\n') {
      printu("\n");
      buf[len] = '\0';
      return len;
    }

    if (ch == 8 || ch == 127) {
      if (len > 0) {
        len--;
        printu("\b \b");
      }
      continue;
    }

    if (ch < 32 || ch > 126) continue;

    if (len + 1 < maxlen) {
      buf[len++] = (char)ch;
      char out[2];
      out[0] = (char)ch;
      out[1] = '\0';
      printu("%s", out);
    }
  }
}

static int parse_line(char *line, char *argv[], int *background) {
  int argc = 0;
  char *p = line;

  *background = 0;

  while (*p) {
    while (*p && is_space(*p)) p++;
    if (*p == '\0') break;

    if (argc >= SHELL_MAX_ARGS) {
      printu("too many arguments, max=%d\n", SHELL_MAX_ARGS);
      return -1;
    }

    if (*p == '"') {
      p++;
      argv[argc++] = p;
      while (*p && *p != '"') p++;
      if (*p == '"') {
        *p = '\0';
        p++;
      }
    } else {
      argv[argc++] = p;
      while (*p && !is_space(*p)) p++;
      if (*p) {
        *p = '\0';
        p++;
      }
    }
  }

  if (argc > 0 && strcmp(argv[argc - 1], "&") == 0) {
    *background = 1;
    argc--;
  }

  return argc;
}

static const char *resolve_command(const char *cmd) {
  if (cmd[0] == '/' || cmd[0] == '.') return cmd;

  for (int i = 0; g_aliases[i].name; i++) {
    if (strcmp(g_aliases[i].name, cmd) == 0) return g_aliases[i].path;
  }

  return 0;
}

static void print_help(void)
{
  printu("============================================================\n");
  printu("|                       PKE Shell Help                     |\n");
  printu("============================================================\n");
  printu("| Builtins   : help pwd cd history jobs wait waitall       |\n");
  printu("|              source exit                                 |\n");
  printu("|----------------------------------------------------------|\n");
  printu("| Aliases    : ls mkdir touch cat writehello backtrace     |\n");
  printu("|              errorline heap alloc0 alloc1 waitdemo sem   |\n");
  printu("|              cow relative execdemo app0 app1 sum         |\n");
  printu("|----------------------------------------------------------|\n");
  printu("| Background : append '&' at end of command                |\n");
  printu("============================================================\n");
}
static int run_script(const char *path);

static int run_external_command(int argc, char *argv[], int background,
                                const char *raw_line) {
  const char *path = resolve_command(argv[0]);
  if (!path) {
    printu("unknown command: %s\n", argv[0]);
    return COMMAND_OK;
  }

  char *exec_argv[SHELL_MAX_ARGS];
  exec_argv[0] = (char *)path;
  for (int i = 1; i < argc; i++) exec_argv[i] = argv[i];

  int pid = fork();
  if (pid < 0) {
    printu("fork failed\n");
    return COMMAND_OK;
  }

  if (pid == 0) {
    int ret = exec(path, argc, exec_argv);
    if (ret == -1) printu("exec failed: %s\n", path);
    exit(-1);
    return COMMAND_OK;
  }

  if (background) {
    add_job(pid, raw_line);
  } else {
    waitpid(pid, 0);
  }

  return COMMAND_OK;
}

static int execute_line(char *line, int interactive) {
  char raw_line[SHELL_MAX_LINE];
  char *argv[SHELL_MAX_ARGS];
  int background = 0;

  (void)interactive;

  safestrcpy(raw_line, line, sizeof(raw_line));

  while (*line && is_space(*line)) line++;
  if (*line == '\0' || *line == '#') return COMMAND_OK;

  add_history(raw_line);

  int argc = parse_line(line, argv, &background);
  if (argc <= 0) return COMMAND_OK;

  if (strcmp(argv[0], "END") == 0) {
    if (!interactive) return COMMAND_EXIT;
    return COMMAND_OK;
  }

  if (strcmp(argv[0], "help") == 0) {
    print_help();
    return COMMAND_OK;
  }

  if (strcmp(argv[0], "pwd") == 0) {
    char cwd[32];
    read_cwd(cwd);
    printu("%s\n", cwd);
    return COMMAND_OK;
  }

  if (strcmp(argv[0], "cd") == 0) {
    if (argc < 2) {
      printu("usage: cd <path>\n");
      return COMMAND_OK;
    }
    if (change_cwd(argv[1]) != 0) printu("cd failed: %s\n", argv[1]);
    return COMMAND_OK;
  }

  if (strcmp(argv[0], "history") == 0) {
    print_history();
    return COMMAND_OK;
  }

  if (strcmp(argv[0], "jobs") == 0) {
    print_jobs();
    return COMMAND_OK;
  }

  if (strcmp(argv[0], "wait") == 0) {
    if (argc < 2) {
      printu("usage: wait <pid>\n");
      return COMMAND_OK;
    }
    int pid = (int)atol(argv[1]);
    int ret = waitpid(pid, 0);
    if (ret > 0) finish_job(ret, 1);
    else printu("wait failed for pid %d\n", pid);
    return COMMAND_OK;
  }

  if (strcmp(argv[0], "waitall") == 0) {
    while (active_job_count() > 0) {
      int pid = waitpid(-1, 0);
      if (pid <= 0) break;
      finish_job(pid, 1);
    }
    return COMMAND_OK;
  }

  if (strcmp(argv[0], "source") == 0) {
    if (argc < 2) {
      printu("usage: source <file>\n");
      return COMMAND_OK;
    }
    return run_script(argv[1]);
  }

  if (strcmp(argv[0], "exit") == 0) {
    if (active_job_count() > 0)
      printu("waiting for background jobs before exit...\n");
    while (active_job_count() > 0) {
      int pid = waitpid(-1, 0);
      if (pid <= 0) break;
      finish_job(pid, 1);
    }
    return COMMAND_EXIT;
  }

  return run_external_command(argc, argv, background, raw_line);
}

static int run_script(const char *path) {
  char buf[SHELL_MAX_SCRIPT];
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    printu("source failed: %s\n", path);
    return COMMAND_OK;
  }

  int n = read_u(fd, buf, SHELL_MAX_SCRIPT - 1);
  close(fd);
  if (n < 0) {
    printu("read failed: %s\n", path);
    return COMMAND_OK;
  }

  buf[n] = '\0';

  char *line = buf;
  while (*line) {
    char *next = line;
    while (*next && *next != '\n' && *next != '\r') next++;

    char saved = *next;
    *next = '\0';

    int ret = execute_line(line, 0);
    if (ret == COMMAND_EXIT) return COMMAND_EXIT;

    if (saved == '\0') break;
    next++;
    if (saved == '\r' && *next == '\n') next++;
    line = next;
  }

  return COMMAND_OK;
}

static void repl_loop(void) {
  char line[SHELL_MAX_LINE];

  while (1) {
    reap_background_jobs();
    print_prompt();
    read_line(line, sizeof(line));
    if (execute_line(line, 1) == COMMAND_EXIT) break;
  }
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  printu("\n======== PKE Interactive Shell ========\n");
  //if (run_script("/shellrc") == COMMAND_EXIT) {
  //  exit(0);
  //  return 0;
  //}

  printu("type 'help' to list commands\n");
  repl_loop();

  exit(0);
  return 0;
}
