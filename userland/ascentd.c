#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ASCENTD_DEFAULT_DIR "/etc/ascentd"
#define ASCENTD_DEFAULT_LOG "/tmp/ascentd.log"
#define MAX_LINE 512
#define MAX_ARGS 32
#define MAX_SERVICES 64

typedef enum {
  SERVICE_ONESHOT,
  SERVICE_BACKGROUND,
  SERVICE_FOREGROUND,
} service_type_t;

typedef struct {
  char name[64];
  char description[160];
  char command[256];
  service_type_t type;
  bool enabled;
  bool respawn;
} service_t;

static const char *ascentd_dir;
static char service_dir[256];
static char target_path[256];
static const char *log_path;

static void log_msg(const char *fmt, ...) {
  va_list ap;

  printf("[AscentD] ");
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  putchar('\n');
  fflush(stdout);

  FILE *log = fopen(log_path, "a");
  if (!log)
    return;

  fprintf(log, "[AscentD] ");
  va_start(ap, fmt);
  vfprintf(log, fmt, ap);
  va_end(ap);
  fputc('\n', log);
  fclose(log);
}

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s))
    s++;

  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1]))
    *--end = '\0';

  if ((*s == '"' && end > s + 1 && end[-1] == '"') ||
      (*s == '\'' && end > s + 1 && end[-1] == '\'')) {
    s++;
    end[-1] = '\0';
  }

  return s;
}

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static bool yes_value(const char *s) {
  return streq(s, "yes") || streq(s, "true") || streq(s, "1");
}

static void normalize_service_name(const char *input, char *out,
                                   size_t out_len) {
  snprintf(out, out_len, "%s", input);
  size_t len = strlen(out);
  if (len > 8 && strcmp(out + len - 8, ".service") == 0)
    out[len - 8] = '\0';
}

static void service_path(const char *name, char *out, size_t out_len) {
  snprintf(out, out_len, "%s/%s.service", service_dir, name);
}

static service_type_t parse_type(const char *value) {
  if (streq(value, "background"))
    return SERVICE_BACKGROUND;
  if (streq(value, "foreground"))
    return SERVICE_FOREGROUND;
  return SERVICE_ONESHOT;
}

static int load_service(const char *name, service_t *svc, bool allow_disabled) {
  char normalized_name[64];
  normalize_service_name(name, normalized_name, sizeof(normalized_name));

  memset(svc, 0, sizeof(*svc));
  snprintf(svc->name, sizeof(svc->name), "%s", normalized_name);
  svc->type = SERVICE_ONESHOT;
  svc->enabled = true;

  char path[512];
  service_path(normalized_name, path, sizeof(path));

  FILE *file = fopen(path, "r");
  if (!file) {
    log_msg("missing service: %s", normalized_name);
    return -1;
  }

  char line[MAX_LINE];
  while (fgets(line, sizeof(line), file)) {
    char *entry = trim(line);
    if (*entry == '\0' || *entry == '#')
      continue;

    char *eq = strchr(entry, '=');
    if (!eq)
      continue;
    *eq = '\0';

    char *key = trim(entry);
    char *value = trim(eq + 1);

    if (streq(key, "NAME"))
      snprintf(svc->name, sizeof(svc->name), "%s", value);
    else if (streq(key, "DESCRIPTION"))
      snprintf(svc->description, sizeof(svc->description), "%s", value);
    else if (streq(key, "COMMAND"))
      snprintf(svc->command, sizeof(svc->command), "%s", value);
    else if (streq(key, "TYPE"))
      svc->type = parse_type(value);
    else if (streq(key, "ENABLED"))
      svc->enabled = !streq(value, "no") && !streq(value, "false") &&
                     !streq(value, "0");
    else if (streq(key, "RESPAWN"))
      svc->respawn = yes_value(value);
  }

  fclose(file);

  if (!svc->enabled && !allow_disabled) {
    log_msg("skipping disabled service: %s", normalized_name);
    return 1;
  }

  if (svc->command[0] == '\0') {
    log_msg("service has no command: %s", normalized_name);
    return -1;
  }

  return 0;
}

static int split_command(char *command, char **argv, int max_args) {
  int argc = 0;
  char *p = command;

  while (*p && argc < max_args - 1) {
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p)
      break;

    char quote = 0;
    if (*p == '"' || *p == '\'')
      quote = *p++;

    argv[argc++] = p;

    if (quote) {
      while (*p && *p != quote)
        p++;
    } else {
      while (*p && !isspace((unsigned char)*p))
        p++;
    }

    if (*p)
      *p++ = '\0';
  }

  argv[argc] = NULL;
  return argc;
}

static int builtin_runtime_dirs(void) {
  mkdir("/tmp", 01777);
  chmod("/tmp", 01777);
  mkdir("/tmp/.X11-unix", 01777);
  chmod("/tmp/.X11-unix", 01777);
  unlink("/tmp/.X0-lock");
  unlink("/tmp/.X11-unix/X0");
  return 0;
}

static void exec_command(const service_t *svc) {
  if (streq(svc->command, "ascentd:runtime-dirs"))
    _exit(builtin_runtime_dirs());

  char command[sizeof(svc->command)];
  char *argv[MAX_ARGS];
  snprintf(command, sizeof(command), "%s", svc->command);

  int argc = split_command(command, argv, MAX_ARGS);
  if (argc == 0)
    _exit(127);

  execvp(argv[0], argv);

  if (errno == ENOEXEC) {
    char *script_argv[MAX_ARGS + 2];
    script_argv[0] = "/bin/sh";
    for (int i = 0; i < argc && i < MAX_ARGS; i++)
      script_argv[i + 1] = argv[i];
    script_argv[argc + 1] = NULL;
    execv("/bin/sh", script_argv);
  }

  perror("ascentd exec");
  _exit(127);
}

static int wait_for(pid_t pid) {
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      return 127;
  }

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 127;
}

static void write_pid_file(const char *service_name, pid_t pid) {
  char path[128];
  snprintf(path, sizeof(path), "/tmp/ascentd-%s.pid", service_name);

  FILE *file = fopen(path, "w");
  if (!file)
    return;
  fprintf(file, "%ld\n", (long)pid);
  fclose(file);
}

static int run_service_process(const service_t *svc, bool wait_child) {
  pid_t pid = fork();
  if (pid < 0) {
    log_msg("%s fork failed: %s", svc->name, strerror(errno));
    return 127;
  }

  if (pid == 0)
    exec_command(svc);

  write_pid_file(svc->name, pid);

  if (!wait_child) {
    log_msg("%s running as pid %ld", svc->name, (long)pid);
    return 0;
  }

  int rc = wait_for(pid);
  log_msg("%s exited with status %d", svc->name, rc);
  return rc;
}

static int start_service(const char *service_name, bool allow_disabled) {
  service_t svc;
  int loaded = load_service(service_name, &svc, allow_disabled);
  if (loaded != 0)
    return loaded > 0 ? 0 : 1;

  if (svc.description[0])
    log_msg("starting %s: %s", svc.name, svc.description);
  else
    log_msg("starting %s", svc.name);

  if (svc.type == SERVICE_BACKGROUND) {
    if (svc.respawn) {
      pid_t pid = fork();
      if (pid < 0) {
        log_msg("%s supervisor fork failed: %s", svc.name, strerror(errno));
        return 127;
      }
      if (pid == 0) {
        for (;;) {
          int rc = run_service_process(&svc, true);
          log_msg("%s stopped with status %d; restarting in 2s", svc.name, rc);
          sleep(2);
        }
      }
      write_pid_file(svc.name, pid);
      log_msg("%s supervisor running as pid %ld", svc.name, (long)pid);
      return 0;
    }

    return run_service_process(&svc, false);
  }

  if (svc.respawn) {
    for (;;) {
      int rc = run_service_process(&svc, true);
      log_msg("%s stopped with status %d; restarting in 2s", svc.name, rc);
      sleep(2);
    }
  }

  return run_service_process(&svc, true);
}

static void usage(void) {
  puts("AscentD service manager");
  puts("usage: ascentd [boot|help|list|start SERVICE|status SERVICE]");
  puts("examples:");
  puts("  ascentd list");
  puts("  ascentd start wayland");
  puts("  ascentd start wayland.service");
  puts("  ascentd status wayland");
}

static void list_services(void) {
  DIR *dir = opendir(service_dir);
  if (!dir)
    return;

  struct dirent *entry;
  while ((entry = readdir(dir))) {
    const char *name = entry->d_name;
    size_t len = strlen(name);
    if (len <= 8 || strcmp(name + len - 8, ".service") != 0)
      continue;

    char service_name[128];
    snprintf(service_name, sizeof(service_name), "%s", name);
    service_name[len - 8] = '\0';
    puts(service_name);
  }

  closedir(dir);
}

static void status_service(const char *service_name) {
  char normalized_name[64];
  normalize_service_name(service_name, normalized_name, sizeof(normalized_name));

  char path[128];
  snprintf(path, sizeof(path), "/tmp/ascentd-%s.pid", normalized_name);

  FILE *file = fopen(path, "r");
  if (!file) {
    printf("%s: no pid file\n", normalized_name);
    return;
  }

  char pid[32];
  if (fgets(pid, sizeof(pid), file))
    printf("%s: started pid %s", normalized_name, pid);
  else
    printf("%s: pid file is empty\n", normalized_name);
  fclose(file);
}

static int load_target(char services[][64], int max_services) {
  int count = 0;

  FILE *file = fopen(target_path, "r");
  if (!file) {
    snprintf(services[count++], 64, "%s", "system-init");
    snprintf(services[count++], 64, "%s", "console");
    return count;
  }

  char line[MAX_LINE];
  while (fgets(line, sizeof(line), file)) {
    char *entry = trim(line);
    if (*entry == '\0' || *entry == '#')
      continue;

    char *eq = strchr(entry, '=');
    if (!eq)
      continue;
    *eq = '\0';

    char *key = trim(entry);
    char *value = trim(eq + 1);
    if (!streq(key, "SERVICES"))
      continue;

    char *tok = strtok(value, " \t\r\n");
    while (tok && count < max_services) {
      snprintf(services[count++], 64, "%s", tok);
      tok = strtok(NULL, " \t\r\n");
    }
  }

  fclose(file);
  return count;
}

static int boot_target(void) {
  mkdir("/tmp", 01777);
  FILE *log = fopen(log_path, "w");
  if (log)
    fclose(log);

  setenv("PATH",
         "/usr/bin:/usr/local/bin:/opt/coreutils/bin:/bin:/opt/bash/bin:"
         "/opt/tcc/bin",
         1);

  log_msg("AscentD boot starting");

  char services[MAX_SERVICES][64];
  int count = load_target(services, MAX_SERVICES);
  for (int i = 0; i < count; i++)
    start_service(services[i], false);

  log_msg("boot target is running");
  for (;;)
    pause();
  return 0;
}

int main(int argc, char **argv) {
  ascentd_dir = getenv("ASCENTD_DIR");
  if (!ascentd_dir)
    ascentd_dir = ASCENTD_DEFAULT_DIR;

  const char *service_dir_env = getenv("ASCENTD_SERVICE_DIR");
  if (service_dir_env)
    snprintf(service_dir, sizeof(service_dir), "%s", service_dir_env);
  else
    snprintf(service_dir, sizeof(service_dir), "%s/services", ascentd_dir);

  const char *target_env = getenv("ASCENTD_TARGET");
  if (target_env)
    snprintf(target_path, sizeof(target_path), "%s", target_env);
  else
    snprintf(target_path, sizeof(target_path), "%s/default.target", ascentd_dir);

  log_path = getenv("ASCENTD_LOG");
  if (!log_path)
    log_path = ASCENTD_DEFAULT_LOG;

  const char *cmd = argc > 1 ? argv[1] : "boot";
  if (streq(cmd, "boot"))
    return boot_target();
  if (streq(cmd, "help") || streq(cmd, "--help") || streq(cmd, "-h")) {
    usage();
    return 0;
  }
  if (streq(cmd, "list")) {
    list_services();
    return 0;
  }
  if (streq(cmd, "start")) {
    if (argc < 3) {
      usage();
      return 2;
    }
    return start_service(argv[2], true);
  }
  if (streq(cmd, "status")) {
    if (argc < 3) {
      usage();
      return 2;
    }
    status_service(argv[2]);
    return 0;
  }

  usage();
  return 2;
}
