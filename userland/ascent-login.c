#include <crypt.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == 10 || s[n - 1] == 13)) s[--n] = 0;
}

static int authenticate(const struct passwd *pw) {
    if (!pw->pw_passwd || strcmp(pw->pw_passwd, "!") == 0 ||
        strcmp(pw->pw_passwd, "*") == 0) return 0;
    if (pw->pw_passwd[0] == 0) return 1;
    char *password = getpass("Password: ");
    if (!password) return 0;
    char *hash = crypt(password, pw->pw_passwd);
    int ok = hash && strcmp(hash, pw->pw_passwd) == 0;
    memset(password, 0, strlen(password));
    return ok;
}

static void run_session(const struct passwd *pw) {
    if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0) {
        perror("login: groups");
        return;
    }
    if (setuid(pw->pw_uid) < 0) {
        perror("login: setuid");
        return;
    }
    if (chdir(pw->pw_dir) < 0 && chdir("/") < 0) {
        perror("login: chdir");
        _exit(1);
    }

    const char *shell = pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
    clearenv();
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", shell, 1);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/opt/coreutils/bin:/usr/bin:/sbin:/bin", 1);
    setenv("TERM", "xterm-256color", 1);
    setenv("TERM_PROGRAM", "vt", 1);

    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    char login_argv0[260];
    snprintf(login_argv0, sizeof(login_argv0), "-%s", base);
    execl(shell, login_argv0, (char *)NULL);
    perror("login: exec shell");
    _exit(127);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    for (;;) {
        char username[64];
        puts("\nAscentOS login");
        fputs("login: ", stdout);
        if (!fgets(username, sizeof(username), stdin)) {
            clearerr(stdin);
            sleep(1);
            continue;
        }
        trim_newline(username);
        if (!username[0]) continue;

        struct passwd *found = getpwnam(username);
        if (!found || !authenticate(found)) {
            puts("Login incorrect");
            sleep(1);
            continue;
        }

        struct passwd pw = *found;
        char name[64], home[256], shell[256];
        snprintf(name, sizeof(name), "%s", found->pw_name);
        snprintf(home, sizeof(home), "%s", found->pw_dir);
        snprintf(shell, sizeof(shell), "%s", found->pw_shell);
        pw.pw_name = name; pw.pw_dir = home; pw.pw_shell = shell;

        pid_t child = fork();
        if (child == 0) { run_session(&pw); _exit(1); }
        if (child < 0) perror("login: fork");
        else { int status; while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
    }
}
