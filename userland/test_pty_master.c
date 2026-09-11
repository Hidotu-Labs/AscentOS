// AvoryOS - PTY master data path, driven the way a terminal emulator drives it
//
// Konsole keeps the master non-blocking and only ever reads it when the Qt event
// dispatcher says it is readable, while st/alacritty watch the same descriptor
// from an epoll loop or a blocking read thread.  A shell whose output never
// reaches the screen while it happily *runs* what you type (it exec'd ls, so it
// was alive and reading) is exactly what a broken slave->master notification
// looks like from the outside.
//
// So this replays what kpty does - open /dev/ptmx, unlock it, name the slave,
// spawn a shell on it with setsid() + TIOCSCTTY + dup2() - and then waits for
// the shell's output the way each kind of terminal does it: poll(2) for a
// hand-rolled loop, ppoll(2) for QSocketNotifier and the Qt event dispatcher,
// epoll for mio/alacritty.  Reads are repeated with several buffer sizes,
// because QIODevice::readAll() asks for far more than the pty buffer holds while
// a simple terminal asks for a pipe buffer's worth.  Output must arrive, input
// written to the master must reach the shell's stdin, and once the shell is gone
// the master has to report the hangup instead of waiting forever.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif
#ifndef TIOCPKT
#define TIOCPKT 0x5420
#endif
#define PTY_BUFFER_SIZE 4096 /* keep in sync with kernel/src/drivers/pty.h */

enum wait_method { WAIT_POLL, WAIT_PPOLL, WAIT_EPOLL };

static int failures = 0;

static void check(int ok, const char *what) {
  printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    failures++;
}

static long elapsed_ms(const struct timespec *from, const struct timespec *to) {
  return (to->tv_sec - from->tv_sec) * 1000L +
         (to->tv_nsec - from->tv_nsec) / 1000000L;
}

/* One readiness wait, by the requested mechanism.  Returns >0 and sets *revents
 * when the descriptor is ready, 0 on timeout and -1 on error. */
static int wait_ready(enum wait_method method, int fd, int epfd, int ms,
                      short *revents) {
  *revents = 0;
  struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
  switch (method) {
  case WAIT_POLL:
    if (poll(&pfd, 1, ms) > 0) { *revents = pfd.revents; return 1; }
    return pfd.revents ? 1 : 0;
  case WAIT_PPOLL: {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    sigset_t mask;
    sigemptyset(&mask);
    int r;
    do {
      r = ppoll(&pfd, 1, &ts, &mask);
    } while (r < 0 && errno == EINTR);
    if (r > 0) { *revents = pfd.revents; return 1; }
    if (r < 0) { printf("   ppoll error: %s\n", strerror(errno)); return -1; }
    return 0;
  }
  case WAIT_EPOLL: {
    struct epoll_event ev;
    int r = epoll_wait(epfd, &ev, 1, ms);
    if (r > 0) { *revents = (short)ev.events; return 1; }
    return 0;
  }
  }
  return -1;
}

/* Reads from the master until `needle` shows up or the deadline passes, using
 * `chunk` bytes per read() the way the caller's read loop would. */
/* kpty_gate reproduces kpty's read path exactly: on a read notification it asks
 * TIOCINQ/FIONREAD how much is pending and *skips the read entirely* when that
 * ioctl fails, so a terminal without the ioctl shows nothing while the shell
 * keeps running.  With the gate on, this asks before every read. */
static int master_read_until(enum wait_method method, int master, int epfd,
                             const char *needle, char *out, size_t out_size,
                             int timeout_ms, size_t chunk, char *buf,
                             int kpty_gate) {
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  size_t len = strlen(out);
  while (!strstr(out, needle)) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    int left = timeout_ms - (int)elapsed_ms(&start, &now);
    if (left <= 0)
      break;
    short revents = 0;
    int ready = wait_ready(method, master, epfd, left, &revents);
    if (ready <= 0)
      break;
    ssize_t want = (ssize_t)chunk;
    if (kpty_gate) {
      int pending = -1;
      if (ioctl(master, TIOCINQ, &pending) != 0) {
        printf("   TIOCINQ/FIONREAD failed: %s\n", strerror(errno));
        return 0;
      }
      if (pending <= 0)
        continue;
      if (pending < want)
        want = pending;
    }
    ssize_t n = read(master, buf, (size_t)want);
    if (n > 0) {
      if (len + (size_t)n < out_size - 1) {
        memcpy(out + len, buf, (size_t)n);
        len += (size_t)n;
        out[len] = '\0';
      }
    } else if (n == 0) {
      break;
    } else if (errno != EAGAIN && errno != EIO) {
      printf("   read(chunk=%zu) failed: %s\n", chunk, strerror(errno));
      break;
    }
  }
  return strstr(out, needle) != NULL;
}

/* qt_close_stdio reproduces what QProcess does before kpty gets a chance to
 * attach the terminal: with the default ClosedChannels mode the child's 0, 1 and
 * 2 are closed first, and only then does the modifier dup2() the pty slave over
 * them.  A dup2() onto a closed low descriptor that lands anywhere other than
 * the requested number leaves the shell reading the terminal but writing
 * somewhere else entirely: typed commands still run, nothing is ever displayed. */
static void run_session(enum wait_method method, int qt_close_stdio,
                        size_t chunk, int kpty_gate, const char *label) {
  char name[160];
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0) { check(0, "posix_openpt(O_RDWR | O_NOCTTY)"); return; }
  int unlock = 0;
  if (ioctl(master, TIOCSPTLCK, &unlock) != 0) { check(0, "unlock the master"); return; }
  char *slave_name = ptsname(master);
  if (!slave_name) { check(0, "ptsname() names the slave"); return; }
  int slave = open(slave_name, O_RDWR | O_NOCTTY);
  if (slave < 0) { check(0, "open() the unlocked slave"); return; }
  /* kpty hands the master to the terminal side non-blockingly. */
  fcntl(master, F_SETFL, O_NONBLOCK);

  int epfd = -1;
  if (method == WAIT_EPOLL) {
    epfd = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = master};
    if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, master, &ev) != 0) {
      check(0, "epoll_ctl(EPOLL_CTL_ADD) on the master");
      return;
    }
  }

  pid_t child = fork();
  if (child == 0) {
    close(master);
    if (epfd >= 0)
      close(epfd);
    if (setsid() < 0)
      _exit(20);
    if (ioctl(slave, TIOCSCTTY, 0) < 0)
      _exit(21);
    if (qt_close_stdio) {
      close(STDIN_FILENO);
      close(STDOUT_FILENO);
      close(STDERR_FILENO);
    }
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO)
      close(slave);
    execl("/bin/sh", "sh", "-c",
          "echo PTY_MARKER_OK; read -r line; echo GOT $line; exit 0",
          (char *)NULL);
    _exit(127);
  }
  snprintf(name, sizeof(name), "%s: session child spawned", label);
  check(child > 0, name);
  if (child <= 0)
    return;

  /* The parent must drop its own slave reference, otherwise the master can
   * never observe the hangup. */
  close(slave);

  char *buf = malloc(chunk);
  char seen[4096];
  seen[0] = '\0';
  if (!buf) { check(0, "malloc of the read buffer"); return; }

  snprintf(name, sizeof(name), "%s: shell output reaches the master", label);
  int got_output = master_read_until(method, master, epfd, "PTY_MARKER_OK", seen,
                                    sizeof(seen), 8000, chunk, buf, kpty_gate);
  check(got_output, name);

  if (got_output) {
    ssize_t wrote = write(master, "hello\n", 6);
    snprintf(name, sizeof(name), "%s: input reaches the shell, reply comes back", label);
    check(wrote == 6 && master_read_until(method, master, epfd, "GOT hello", seen,
                                         sizeof(seen), 8000, chunk, buf, kpty_gate),
          name);
  }

  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  int hung_up = 0;
  while (1) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (elapsed_ms(&start, &now) > 8000)
      break;
    short revents = 0;
    int ready = wait_ready(method, master, epfd, 250, &revents);
    if (ready <= 0)
      continue;
    ssize_t n = read(master, buf, chunk);
    if (n == 0 || (n < 0 && errno == EIO) || (revents & (EPOLLHUP | POLLHUP))) {
      hung_up = 1;
      break;
    }
  }
  snprintf(name, sizeof(name), "%s: master reports the hangup when the shell exits", label);
  check(hung_up, name);

  int status = 0;
  snprintf(name, sizeof(name), "%s: session child is reapable", label);
  check(waitpid(child, &status, 0) == child, name);
  snprintf(name, sizeof(name), "%s: shell exited cleanly", label);
  check(WIFEXITED(status) && WEXITSTATUS(status) == 0, name);

  if (failures)
    printf("   transcript: %s\n", seen);
  free(buf);
  if (epfd >= 0)
    close(epfd);
  close(master);
}

int main(void) {
  printf("--- test_pty_master ---\n");
  run_session(WAIT_POLL, 0, 256, 0, "poll 256B");
  run_session(WAIT_PPOLL, 0, 256, 0, "ppoll 256B");
  run_session(WAIT_EPOLL, 0, 256, 0, "epoll 256B");
  run_session(WAIT_PPOLL, 1, 256, 0, "ppoll qt-child");
  run_session(WAIT_PPOLL, 0, PTY_BUFFER_SIZE, 0, "ppoll 4096B");
  run_session(WAIT_PPOLL, 0, 65536, 0, "ppoll 64KiB");
  run_session(WAIT_POLL, 0, 65536, 0, "poll 64KiB");
  /* The exact shape kpty uses: ppoll, then TIOCINQ, then read of that size. */
  run_session(WAIT_PPOLL, 1, 65536, 1, "kpty gate FIONREAD");
  printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
