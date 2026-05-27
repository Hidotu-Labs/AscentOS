#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

int main() {
    printf("Testing AF_UNIX socketpair read behavior...\n");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }

    int flags = fcntl(sv[0], F_GETFL, 0);
    fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);

    char buf[10];
    ssize_t n = read(sv[0], buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN) {
            printf("[OK] Non-blocking read on empty socket returned EAGAIN\n");
        } else {
            printf("[FAIL] Non-blocking read on empty socket returned %zd (errno=%d: %s)\n", n, errno, strerror(errno));
        }
    } else if (n == 0) {
        printf("[FAIL] Non-blocking read on empty socket returned 0 (EOF)!\n");
    } else {
        printf("[FAIL] Non-blocking read on empty socket returned %zd bytes?!\n", n);
    }

    printf("\nTesting eventfd read behavior...\n");
    int efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) {
        perror("eventfd");
        return 1;
    }

    uint64_t val;
    n = read(efd, &val, sizeof(val));
    if (n < 0) {
        if (errno == EAGAIN) {
            printf("[OK] Non-blocking read on empty eventfd returned EAGAIN\n");
        } else {
            printf("[FAIL] Non-blocking read on empty eventfd returned %zd (errno=%d: %s)\n", n, errno, strerror(errno));
        }
    } else if (n == 0) {
        printf("[FAIL] Non-blocking read on empty eventfd returned 0 (EOF)!\n");
    } else {
        printf("[FAIL] Non-blocking read on empty eventfd returned %zd bytes?!\n", n);
    }

    return 0;
}
