// AvoryOS - open() flag semantics and KDE/KIO worker socket reproduction
//
// Qt's QTemporaryFile asks for an unnamed temporary file with
//   open(dir, O_TMPFILE | O_RDWR, 0600)
// and only falls back to a named template file when that fails with
// EOPNOTSUPP or EISDIR.  If the kernel happily returns a descriptor for the
// *directory*, Qt believes it has an anonymous file, so QTemporaryFile::fileName()
// comes back empty.  KIO feeds that empty name straight into
// QLocalServer::listen(), which refuses it ("QLocalServer::listen: Name error")
// and every KIO worker (file:, trash:, tags:) fails to start in Dolphin.
//
// This test pins down the three open() behaviours that stack on top of each
// other here: O_TMPFILE must not be silently accepted, directories must not be
// openable for writing, and O_CREAT|O_EXCL must report EEXIST.  The last check
// replays the exact KIO sequence: create the socket file, unlink it, bind(),
// listen(), connect(), accept() and exchange a byte.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0200000
#endif
#ifndef O_TMPFILE
#define O_TMPFILE (020000000 | O_DIRECTORY)
#endif
#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP 95
#endif

static int failures = 0;

static const char *errname(int err) {
    switch (err) {
    case 0:            return "0";
    case EPERM:        return "EPERM";
    case ENOENT:       return "ENOENT";
    case EIO:          return "EIO";
    case EBADF:        return "EBADF";
    case EACCES:       return "EACCES";
    case EEXIST:       return "EEXIST";
    case EINVAL:       return "EINVAL";
    case EMFILE:       return "EMFILE";
    case ENOTDIR:      return "ENOTDIR";
    case EISDIR:       return "EISDIR";
    case EOPNOTSUPP:   return "EOPNOTSUPP";
    case EADDRINUSE:   return "EADDRINUSE";
    case ECONNREFUSED: return "ECONNREFUSED";
    default:           return "other";
    }
}

static void pass(const char *what) { printf("  [PASS] %s\n", what); }

static void fail(const char *what, int err) {
    failures++;
    printf("  [FAIL] %s (errno=%d %s)\n", what, err, errname(err));
}

// open() must fail; returns 1 when it did with one of the accepted errnos.
static int open_fails(const char *path, int flags, const char *what, int want_a,
                      int want_b) {
    errno = 0;
    int fd = open(path, flags, 0600);
    if (fd >= 0) {
        close(fd);
        fail(what, 0);
        printf("         expected open() to fail, but it returned fd %d\n", fd);
        return 0;
    }
    int err = errno;
    if (err == want_a || (want_b && err == want_b)) {
        pass(what);
        return 1;
    }
    fail(what, err);
    return 0;
}

// open() must succeed; returns the descriptor or -1.
static int open_ok(const char *path, int flags, const char *what) {
    errno = 0;
    int fd = open(path, flags, 0600);
    if (fd < 0) {
        fail(what, errno);
        return -1;
    }
    pass(what);
    return fd;
}

static void test_open_flags(void) {
    char saved_cwd[256];
    if (!getcwd(saved_cwd, sizeof(saved_cwd)))
        strcpy(saved_cwd, "/");

    printf("\n=== open() flag semantics ===\n");

    // Qt's QTemporaryFile / QSaveFile path: O_TMPFILE must be refused so that
    // Qt falls back to a named temporary file.
    open_fails("/tmp", O_TMPFILE | O_RDWR, "open(/tmp, O_TMPFILE|O_RDWR) refused",
               EOPNOTSUPP, EISDIR);
    open_fails("/tmp", O_TMPFILE | O_WRONLY, "open(/tmp, O_TMPFILE|O_WRONLY) refused",
               EOPNOTSUPP, EISDIR);

    // Qt's QProcess opens the child's working directory with
    // O_RDONLY|O_DIRECTORY|O_PATH, whose bits are identical to O_TMPFILE.  That
    // must stay a working descriptor or Konsole cannot start its shell.
    int pathfd = open_ok("/tmp", O_RDONLY | O_DIRECTORY | O_PATH,
                         "open(/tmp, O_RDONLY|O_DIRECTORY|O_PATH)");
    if (pathfd >= 0) {
        errno = 0;
        if (fchdir(pathfd) == 0)
            pass("fchdir() on the O_PATH directory handle");
        else
            fail("fchdir() on the O_PATH directory handle", errno);
        if (chdir(saved_cwd) != 0)
            perror("restore cwd");
        close(pathfd);
    }

    // POSIX: a directory may only be opened for reading.
    open_fails("/tmp", O_RDWR, "open(/tmp, O_RDWR) -> EISDIR", EISDIR, 0);
    open_fails("/tmp", O_WRONLY, "open(/tmp, O_WRONLY) -> EISDIR", EISDIR, 0);
    int dirfd = open_ok("/tmp", O_RDONLY | O_DIRECTORY, "open(/tmp, O_RDONLY|O_DIRECTORY)");
    if (dirfd >= 0)
        close(dirfd);

    // O_CREAT | O_EXCL on an existing node must not silently succeed.
    const char *existing = "/tmp/test_tmpfile_existing";
    int fd = open(existing, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        fail("create scratch file", errno);
    } else {
        close(fd);
        open_fails(existing, O_CREAT | O_EXCL | O_RDWR,
                   "open(existing, O_CREAT|O_EXCL) -> EEXIST", EEXIST, 0);
        unlink(existing);
    }
}

// Replays ConnectionBackend::listenForRemote(): Qt creates <prefix>/<app>XXXXXX.N.kioworker.socket
// with QTemporaryFile, unlinks it and binds a listening AF_UNIX socket on the name.
static int test_kio_worker_socket(const char *dir, int make_dir) {
    char path[256];
    printf("\n=== KIO worker socket in %s ===\n", dir);

    if (make_dir) {
        errno = 0;
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
            fail("mkdir runtime directory", errno);
            return 0;
        }
    }

    snprintf(path, sizeof(path), "%s/org.kde.dolphinAbCdEf.1.kioworker.socket", dir);

    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        fail("QTemporaryFile::open()", errno);
        return 0;
    }
    pass("QTemporaryFile::open()");
    close(fd);

    struct stat st;
    if (stat(path, &st) != 0) {
        fail("stat() of the new temporary file", errno);
        return 0;
    }
    pass("stat() of the new temporary file");

    if (unlink(path) != 0) {
        fail("QFile::remove() of the temporary file", errno);
        return 0;
    }
    pass("QFile::remove() of the temporary file");

    // The name QLocalServer::listen() would receive must not be empty, and
    // bind() must accept it.
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        fail("socket(AF_UNIX)", errno);
        return 0;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fail("socket name too long for sun_path", ENAMETOOLONG);
        close(sfd);
        return 0;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fail("bind() on the KIO worker socket name", errno);
        close(sfd);
        return 0;
    }
    pass("bind() on the KIO worker socket name");

    if (listen(sfd, 5) != 0) {
        fail("listen()", errno);
        close(sfd);
        return 0;
    }
    pass("listen()");

    // Child acts as the KIO worker: connect, read a byte, echo it back.
    pid_t child = fork();
    if (child == 0) {
        int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cfd < 0)
            _exit(2);
        for (int i = 0; i < 100; i++) {
            if (connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
                break;
            usleep(10000);
            if (i == 99)
                _exit(3);
        }
        char c = 0;
        if (read(cfd, &c, 1) != 1 || c != 'K')
            _exit(4);
        if (write(cfd, "k", 1) != 1)
            _exit(5);
        close(cfd);
        _exit(0);
    }

    int afd = accept(sfd, NULL, NULL);
    if (afd < 0) {
        fail("accept()", errno);
        close(sfd);
        return 0;
    }
    pass("accept()");

    int ok = write(afd, "K", 1) == 1;
    char reply = 0;
    ok = ok && read(afd, &reply, 1) == 1 && reply == 'k';
    if (ok)
        pass("worker handshake round trip");
    else
        fail("worker handshake round trip", EIO);

    close(afd);
    close(sfd);
    unlink(path);

    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failures++;
        printf("  [FAIL] worker child exited with status %d\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        ok = 0;
    }
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== AvoryOS tmpfile / KIO worker socket test ===\n");

    test_open_flags();
    test_kio_worker_socket("/tmp", 0);
    test_kio_worker_socket("/tmp/runtime-root", 1);

    printf("\n%s\n", failures ? "=== TESTS FAILED ===" : "=== ALL TESTS PASSED ===");
    return failures ? 1 : 0;
}
