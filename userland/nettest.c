#define _GNU_SOURCE

/*
 * nettest - network benchmark / stress tool for AvoryOS.
 *
 * Host side is scripts/nettest-server.py (TCP sink/echo/source, UDP echo).
 * The QEMU user network exposes the host at 10.0.2.2, so e.g.:
 *
 *   (host)  ./scripts/netbench.sh server tcp-sink 9000
 *   (guest) nettest tcp-source 10.0.2.2 9000 10
 *
 * Modes:
 *   tcp-source  HOST PORT [SECONDS] [BUF]   saturate a sink
 *   tcp-sink    PORT                        receive until EOF (server)
 *   tcp-echo    PORT                        echo bytes (server)
 *   tcp-churn   HOST PORT COUNT [PAR] [MS]  connect/close churn
 *   ws-sim      HOST PORT CONNS [SECONDS]   hold CONNS sockets, ping/echo
 *   udp-echo    PORT                        echo datagrams (server)
 *   udp-load    HOST PORT COUNT [SIZE]      burst datagrams
 *   udp-voice   HOST PORT [SECONDS]         voice profile: 50 pps, 160 B
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RX_BUF (64 * 1024)

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int make_addr(const char *host, const char *port, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)atoi(port));
    if (inet_pton(AF_INET, host, &sa->sin_addr) != 1) {
        fprintf(stderr, "nettest: bad IPv4 host '%s'\n", host);
        return -1;
    }
    return 0;
}

static int connect_tcp(const char *host, const char *port)
{
    struct sockaddr_in sa;
    if (make_addr(host, port, &sa) < 0)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "nettest: connect %s:%s: %s\n", host, port,
                strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int listen_udp(const char *port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)atoi(port));
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    return fd;
}

static int listen_tcp(const char *port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)atoi(port));
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

/* ---------------- TCP ---------------- */

static int run_tcp_source(const char *host, const char *port, double seconds,
                          size_t bufsize)
{
    int fd = connect_tcp(host, port);
    if (fd < 0)
        return 1;

    char *buf = malloc(bufsize);
    if (!buf) {
        close(fd);
        return 1;
    }
    memset(buf, 'A', bufsize);

    double t0 = now_s();
    unsigned long long total = 0;
    while (now_s() - t0 < seconds) {
        ssize_t n = send(fd, buf, bufsize, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd p = { .fd = fd, .events = POLLOUT };
                poll(&p, 1, 100);
                continue;
            }
            fprintf(stderr, "send: %s\n", strerror(errno));
            break;
        }
        total += (unsigned long long)n;
    }
    double dt = now_s() - t0;
    if (dt <= 0)
        dt = 1e-9;
    printf("tcp-source: %llu bytes in %.2fs = %.3f MB/s (%.2f Mbit/s)\n",
           total, dt, (double)total / dt / 1e6, (double)total * 8.0 / dt / 1e6);
    shutdown(fd, SHUT_WR);
    close(fd);
    free(buf);
    return 0;
}

static int run_tcp_sink(const char *port)
{
    int lfd = listen_tcp(port);
    if (lfd < 0)
        return 1;
    char *buf = malloc(RX_BUF);
    if (!buf) {
        close(lfd);
        return 1;
    }

    printf("tcp-sink: listening on 0.0.0.0:%s\n", port);
    fflush(stdout);

    unsigned long long grand = 0;
    int conns = 0;
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        conns++;
        double t0 = now_s();
        unsigned long long total = 0;
        for (;;) {
            ssize_t n = recv(fd, buf, RX_BUF, 0);
            if (n == 0)
                break;
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "recv: %s\n", strerror(errno));
                break;
            }
            total += (unsigned long long)n;
        }
        close(fd);
        double dt = now_s() - t0;
        if (dt <= 0)
            dt = 1e-9;
        grand += total;
        printf("tcp-sink: conn %d closed, %llu bytes in %.2fs = %.3f MB/s "
               "(total %llu)\n",
               conns, total, dt, (double)total / dt / 1e6, grand);
        fflush(stdout);
    }
    close(lfd);
    free(buf);
    return 0;
}

static int run_tcp_echo(const char *port)
{
    int lfd = listen_tcp(port);
    if (lfd < 0)
        return 1;
    char *buf = malloc(RX_BUF);
    if (!buf) {
        close(lfd);
        return 1;
    }
    printf("tcp-echo: listening on 0.0.0.0:%s\n", port);
    fflush(stdout);

    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        for (;;) {
            ssize_t n = recv(fd, buf, RX_BUF, 0);
            if (n <= 0)
                break;
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = send(fd, buf + off, (size_t)(n - off), 0);
                if (w <= 0)
                    goto next_conn;
                off += w;
            }
        }
    next_conn:
        close(fd);
    }
    close(lfd);
    free(buf);
    return 0;
}

static void drain_close(int fd)
{
    char tmp[512];
    for (int i = 0; i < 4; i++) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n <= 0)
            break;
    }
    close(fd);
}

static int run_tcp_churn(const char *host, const char *port, long count,
                         long parallel, long hold_ms)
{
    struct sockaddr_in sa;
    if (make_addr(host, port, &sa) < 0)
        return 1;
    if (parallel < 1)
        parallel = 1;
    if (parallel > 256)
        parallel = 256;

    int *fds = calloc((size_t)parallel, sizeof(int));
    struct pollfd *pfds = calloc((size_t)parallel, sizeof(struct pollfd));
    if (!fds || !pfds)
        return 1;

    long done = 0;
    long ok = 0, failed = 0;
    double t0 = now_s();

    while (done < count) {
        long batch = count - done;
        if (batch > parallel)
            batch = parallel;

        long opened = 0;
        for (long i = 0; i < batch; i++) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                continue;
            int fl = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
            if (r < 0 && errno != EINPROGRESS) {
                close(fd);
                failed++;
                continue;
            }
            fds[opened++] = fd;
        }

        for (long i = 0; i < opened; i++) {
            pfds[i].fd = fds[i];
            pfds[i].events = POLLOUT;
            pfds[i].revents = 0;
        }
        int ready = poll(pfds, (nfds_t)opened, 3000);
        for (long i = 0; i < opened; i++) {
            if (ready <= 0 || !(pfds[i].revents & (POLLOUT | POLLERR))) {
                failed++;
                close(fds[i]);
                continue;
            }
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &elen) < 0 ||
                err != 0) {
                failed++;
                close(fds[i]);
                continue;
            }
            ok++;
            if (hold_ms > 0) {
                struct timespec ts = { .tv_sec = hold_ms / 1000,
                                       .tv_nsec = (hold_ms % 1000) * 1000000 };
                nanosleep(&ts, NULL);
            }
            drain_close(fds[i]);
        }
        done += batch;
    }

    double dt = now_s() - t0;
    if (dt <= 0)
        dt = 1e-9;
    printf("tcp-churn: %ld connections in %.2fs = %.1f/s (ok=%ld failed=%ld)\n",
           done, dt, (double)done / dt, ok, failed);
    free(fds);
    free(pfds);
    return failed ? 1 : 0;
}

static int run_ws_sim(const char *host, const char *port, long conns,
                      double seconds)
{
    struct sockaddr_in sa;
    if (make_addr(host, port, &sa) < 0)
        return 1;
    if (conns < 1)
        conns = 1;
    if (conns > 1024)
        conns = 1024;

    int *fds = calloc((size_t)conns, sizeof(int));
    struct pollfd *pfds = calloc((size_t)conns, sizeof(struct pollfd));
    if (!fds || !pfds)
        return 1;

    /* Non-blocking connects. */
    long opened = 0;
    for (long i = 0; i < conns; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        if (r < 0 && errno != EINPROGRESS) {
            close(fd);
            continue;
        }
        fds[opened++] = fd;
    }

    for (long i = 0; i < opened; i++) {
        pfds[i].fd = fds[i];
        pfds[i].events = POLLOUT;
        pfds[i].revents = 0;
    }
    int ready = poll(pfds, (nfds_t)opened, 5000);
    long connected = 0, conn_failed = 0;
    for (long i = 0; i < opened; i++) {
        if (ready <= 0 || !(pfds[i].revents & (POLLOUT | POLLERR))) {
            conn_failed++;
            close(fds[i]);
            fds[i] = -1;
            continue;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &elen) < 0 ||
            err != 0) {
            conn_failed++;
            close(fds[i]);
            fds[i] = -1;
            continue;
        }
        connected++;
    }
    printf("ws-sim: connected %ld/%ld (failed %ld)\n", connected, conns,
           conn_failed);
    fflush(stdout);

    const char ping[] = "ping\n";
    char rb[64];
    unsigned long long sent = 0, echoed = 0, rxc = 0, errors = 0;
    double t0 = now_s();
    double next_report = t0 + 1.0;

    while (now_s() - t0 < seconds) {
        double t = now_s();
        for (long i = 0; i < opened; i++) {
            if (fds[i] < 0)
                continue;
            ssize_t n = send(fds[i], ping, sizeof(ping) - 1, 0);
            if (n > 0) {
                sent++;
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != EINTR) {
                errors++;
                close(fds[i]);
                fds[i] = -1;
            }
            for (;;) {
                ssize_t r = recv(fds[i], rb, sizeof(rb), 0);
                if (r > 0) {
                    rxc++;
                    echoed += (unsigned long long)r;
                } else if (r == 0) {
                    close(fds[i]);
                    fds[i] = -1;
                    break;
                } else {
                    break;
                }
            }
        }
        if (t >= next_report) {
            printf("ws-sim: t=%.0fs pings=%llu echoed=%llu rx_chunks=%llu "
                   "errors=%llu\n",
                   t - t0, sent, echoed, rxc, errors);
            fflush(stdout);
            next_report = t + 1.0;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    for (long i = 0; i < opened; i++)
        if (fds[i] >= 0)
            close(fds[i]);
    double dt = now_s() - t0;
    printf("ws-sim: done, pings=%llu echoed=%llu errors=%llu over %.1fs\n",
           sent, echoed, errors, dt);
    free(fds);
    free(pfds);
    return 0;
}

/* ---------------- UDP ---------------- */

static int run_udp_echo(const char *port)
{
    int fd = listen_udp(port);
    if (fd < 0)
        return 1;
    printf("udp-echo: listening on 0.0.0.0:%s\n", port);
    fflush(stdout);

    char buf[2048];
    for (;;) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("recvfrom");
            break;
        }
        sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&src, slen);
    }
    close(fd);
    return 0;
}

static int run_udp_load(const char *host, const char *port, long count,
                        size_t size)
{
    struct sockaddr_in sa;
    if (make_addr(host, port, &sa) < 0)
        return 1;
    if (size > 1472)
        size = 1472;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    char *buf = malloc(size);
    if (!buf)
        return 1;
    memset(buf, 'U', size);

    double t0 = now_s();
    long sent = 0;
    for (long i = 0; i < count; i++) {
        if (sendto(fd, buf, size, 0, (struct sockaddr *)&sa, sizeof(sa)) >= 0)
            sent++;
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            break;
    }
    double dt = now_s() - t0;
    if (dt <= 0)
        dt = 1e-9;
    printf("udp-load: sent %ld/%ld datagrams of %zu B in %.2fs = %.0f pps, "
           "%.2f Mbit/s\n",
           sent, count, size, dt, (double)sent / dt,
           (double)sent * (double)size * 8.0 / dt / 1e6);
    close(fd);
    free(buf);
    return 0;
}

static int run_udp_voice(const char *host, const char *port, double seconds)
{
    struct sockaddr_in sa;
    if (make_addr(host, port, &sa) < 0)
        return 1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    char buf[160];
    memset(buf, 0x5a, sizeof(buf));
    double t0 = now_s();
    long sent = 0;
    double next = t0;

    while (now_s() - t0 < seconds) {
        sendto(fd, buf, sizeof(buf), 0, (struct sockaddr *)&sa, sizeof(sa));
        sent++;
        next += 0.02;
        double now = now_s();
        if (next > now) {
            struct timespec ts;
            double d = next - now;
            ts.tv_sec = (time_t)d;
            ts.tv_nsec = (long)((d - (double)ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        } else {
            next = now;
        }
    }
    double dt = now_s() - t0;
    printf("udp-voice: %ld datagrams in %.2fs = %.1f pps (%.2f kbit/s)\n", sent,
           dt, (double)sent / dt, (double)sent * 160.0 * 8.0 / dt / 1000.0);
    close(fd);
    return 0;
}

/* ---------------- main ---------------- */

static void usage(const char *argv0)
{
    printf(
        "usage:\n"
        "  %s tcp-source HOST PORT [SECONDS] [BUF]\n"
        "  %s tcp-sink PORT\n"
        "  %s tcp-echo PORT\n"
        "  %s tcp-churn HOST PORT COUNT [PARALLEL] [HOLD_MS]\n"
        "  %s ws-sim HOST PORT CONNS [SECONDS]\n"
        "  %s udp-echo PORT\n"
        "  %s udp-load HOST PORT COUNT [SIZE]\n"
        "  %s udp-voice HOST PORT [SECONDS]\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const char *mode = argv[1];

    if (!strcmp(mode, "tcp-source") && argc >= 4) {
        double secs = argc >= 5 ? atof(argv[4]) : 10.0;
        size_t buf = argc >= 6 ? (size_t)strtoul(argv[5], NULL, 0) : 65536;
        return run_tcp_source(argv[2], argv[3], secs ? secs : 10.0, buf);
    }
    if (!strcmp(mode, "tcp-sink") && argc >= 3)
        return run_tcp_sink(argv[2]);
    if (!strcmp(mode, "tcp-echo") && argc >= 3)
        return run_tcp_echo(argv[2]);
    if (!strcmp(mode, "tcp-churn") && argc >= 5) {
        long count = atol(argv[4]);
        long par = argc >= 6 ? atol(argv[5]) : 16;
        long hold = argc >= 7 ? atol(argv[6]) : 0;
        return run_tcp_churn(argv[2], argv[3], count, par, hold);
    }
    if (!strcmp(mode, "ws-sim") && argc >= 5) {
        long conns = atol(argv[4]);
        double secs = argc >= 6 ? atof(argv[5]) : 30.0;
        return run_ws_sim(argv[2], argv[3], conns, secs ? secs : 30.0);
    }
    if (!strcmp(mode, "udp-echo") && argc >= 3)
        return run_udp_echo(argv[2]);
    if (!strcmp(mode, "udp-load") && argc >= 5) {
        long count = atol(argv[4]);
        size_t size = argc >= 6 ? (size_t)strtoul(argv[5], NULL, 0) : 512;
        return run_udp_load(argv[2], argv[3], count, size);
    }
    if (!strcmp(mode, "udp-voice") && argc >= 4) {
        double secs = argc >= 5 ? atof(argv[4]) : 30.0;
        return run_udp_voice(argv[2], argv[3], secs ? secs : 30.0);
    }

    usage(argv[0]);
    return 2;
}
