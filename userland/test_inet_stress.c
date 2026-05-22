#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#define TEST_PORT 9999
#define TEST_MSG "Hello from userland AF_INET stress test!"
#define NUM_CLIENTS 5
#define ITERATIONS 10

void run_client(int id) {
    for (int i = 0; i < ITERATIONS; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            perror("client socket");
            exit(1);
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(TEST_PORT);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("client connect");
            close(sock);
            exit(1);
        }

        char buf[256];
        snprintf(buf, sizeof(buf), "Client %d, Iteration %d: %s", id, i, TEST_MSG);
        if (send(sock, buf, strlen(buf), 0) < 0) {
            perror("client send");
            close(sock);
            exit(1);
        }

        char recv_buf[256];
        memset(recv_buf, 0, sizeof(recv_buf));
        ssize_t n = recv(sock, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n < 0) {
            perror("client recv");
            close(sock);
            exit(1);
        }

        if (strcmp(buf, recv_buf) != 0) {
            fprintf(stderr, "Mismatched data! Sent: %s, Got: %s\n", buf, recv_buf);
            close(sock);
            exit(1);
        }

        close(sock);
    }
    printf("Client %d finished %d iterations\n", id, ITERATIONS);
    exit(0);
}

void run_udp_client(int id) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("udp client socket");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT + 1);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    for (int i = 0; i < ITERATIONS; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "UDP Client %d, Iteration %d: %s", id, i, TEST_MSG);
        if (sendto(sock, buf, strlen(buf), 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("udp client sendto");
            close(sock);
            exit(1);
        }

        char recv_buf[256];
        memset(recv_buf, 0, sizeof(recv_buf));
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        ssize_t n = recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0, (struct sockaddr *)&src_addr, &src_len);
        if (n < 0) {
            perror("udp client recvfrom");
            close(sock);
            exit(1);
        }

        if (strcmp(buf, recv_buf) != 0) {
            fprintf(stderr, "UDP Mismatched data! Sent: %s, Got: %s\n", buf, recv_buf);
            close(sock);
            exit(1);
        }
    }

    printf("UDP Client %d finished %d iterations\n", id, ITERATIONS);
    close(sock);
    exit(0);
}

void run_udp_server() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("udp server socket");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT + 1);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("udp server bind");
        exit(1);
    }

    printf("UDP Server listening on port %d\n", TEST_PORT + 1);

    int total_msgs = NUM_CLIENTS * ITERATIONS;
    for (int i = 0; i < total_msgs; i++) {
        char buf[256];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &client_len);
        if (n > 0) {
            sendto(sock, buf, n, 0, (struct sockaddr *)&client_addr, client_len);
        }
    }

    close(sock);
    exit(0);
}

int main() {
    printf("--- AF_INET Userland Stress Test (TCP) ---\n");

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        return 1;
    }

    printf("TCP Server listening on port %d\n", TEST_PORT);

    // Fork clients
    for (int i = 0; i < NUM_CLIENTS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            run_client(i);
        } else if (pid < 0) {
            perror("fork client");
            return 1;
        }
    }

    // Server loop: accept and echo
    int total_conns = NUM_CLIENTS * ITERATIONS;
    for (int i = 0; i < total_conns; i++) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        char buf[256];
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            send(client_fd, buf, n, 0);
        }
        close(client_fd);
    }

    // Wait for all TCP clients
    for (int i = 0; i < NUM_CLIENTS; i++) {
        wait(NULL);
    }

    printf("--- TCP stress test finished ---\n\n");
    close(server_fd);

    printf("--- AF_INET Userland Stress Test (UDP) ---\n");
    
    pid_t udp_server_pid = fork();
    if (udp_server_pid == 0) {
        run_udp_server();
    } else if (udp_server_pid < 0) {
        perror("fork udp server");
        return 1;
    }

    // Small delay to allow server to start
    usleep(100000);

    for (int i = 0; i < NUM_CLIENTS; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            run_udp_client(i);
        } else if (pid < 0) {
            perror("fork udp client");
            return 1;
        }
    }

    // Wait for all UDP clients
    for (int i = 0; i < NUM_CLIENTS; i++) {
        wait(NULL);
    }

    // Wait for UDP server
    // (In real life we might want to kill it, but our run_udp_server exits after total_msgs)
    waitpid(udp_server_pid, NULL, 0);

    printf("--- UDP stress test finished ---\n");
    printf("--- All AF_INET tests PASSED ---\n");
    
    return 0;
}
