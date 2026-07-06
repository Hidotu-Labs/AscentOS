#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/wait.h>

static int failures;

static void check(int ok, const char *message) {
    printf("  [%s] %s\n", ok ? "OK" : "FAIL", message);
    if (!ok) failures++;
}

static void check_device(const char *path, int flags) {
    int fd = open(path, flags | O_NONBLOCK);
    char message[160];
    snprintf(message, sizeof(message), "non-root session can open %s", path);
    check(fd >= 0, message);
    if (fd >= 0) close(fd);
}

static void check_unix_socket_owner(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address;
    struct stat st;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path),
             "/tmp/.X11-unix/ascent-test-%lu", (unsigned long)getuid());
    unlink(address.sun_path);
    int bound = fd >= 0 &&
        bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0;
    check(bound && stat(address.sun_path, &st) == 0 &&
          st.st_uid == getuid(),
          "filesystem Unix socket inherits the caller UID");
    if (fd >= 0) close(fd);
    check(!bound || unlink(address.sun_path) == 0,
          "user can unlink their filesystem Unix socket");
}

static void check_controlling_tty(void) {
    pid_t child = fork();
    if (child == 0) {
        if (setsid() < 0) _exit(10);
        int fd = open("/dev/tty1", O_RDWR);
        if (fd < 0) _exit(11);
        if (ioctl(fd, TIOCNOTTY, 0) < 0) _exit(12);
        if (ioctl(fd, TIOCSCTTY, 0) < 0) _exit(13);
        close(fd);
        _exit(0);
    }
    if (child < 0) {
        check(0, "fork for TIOCSCTTY test");
        return;
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "session-leader tty open auto-attaches and can reattach /dev/tty1");
}

int main(void) {
    struct stat st;
    char runtime[96];

    puts("=== Graphical Session Permissions Test ===");
    check(stat("/tmp/.X11-unix", &st) == 0 &&
          (st.st_mode & 07777) == 01777,
          "/tmp/.X11-unix reports mode 01777");

    snprintf(runtime, sizeof(runtime), "/tmp/ascent-runtime-%lu",
             (unsigned long)getuid());
    if (mkdir(runtime, 0700) < 0 && errno != EEXIST) {
        perror("mkdir runtime");
        failures++;
    }
    if (chmod(runtime, 0700) < 0) {
        perror("chmod runtime");
        failures++;
    }
    check(stat(runtime, &st) == 0 && (st.st_mode & 0777) == 0700 &&
          st.st_uid == getuid(),
          "Wayland runtime directory is private and user-owned");

    check_device("/dev/dri/card0", O_RDWR);
    check_device("/dev/input/event0", O_RDONLY);
    check_device("/dev/input/event1", O_RDONLY);
    check_unix_socket_owner();
    check_controlling_tty();

    puts(failures ? "Graphical session permissions: FAIL" :
                    "Graphical session permissions: PASS");
    return failures ? 1 : 0;
}
