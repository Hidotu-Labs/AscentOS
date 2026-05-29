#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>

int main() {
    printf("Netlink Test: Starting...\n");

    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1; // multicast group 1 (uevents)

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind");
        close(fd);
        return 1;
    }

    printf("Socket bound to group 1. Waiting for events...\n");

    char buf[4096];
    while (1) {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        if (len < 0) {
            perror("recv");
            break;
        }
        if (len == 0) {
            printf("Connection closed\n");
            break;
        }

        printf("Received Netlink message (%zd bytes):\n", len);
        // Uevents are null-separated strings
        for (ssize_t i = 0; i < len; i++) {
            if (buf[i] == '\0') {
                printf("\n");
            } else {
                putchar(buf[i]);
            }
        }
        printf("----------------------------\n");
    }

    close(fd);
    return 0;
}
