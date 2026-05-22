#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
};

void format_dns_name(unsigned char* dns, unsigned char* host) {
    int lock = 0 , i;
    strcat((char*)host,".");
    for(i = 0 ; i < (int)strlen((char*)host) ; i++) {
        if(host[i]=='.') {
            *dns++ = i-lock;
            for(;lock<i;lock++) {
                *dns++ = host[lock];
            }
            lock++;
        }
    }
    *dns++ = '\0';
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <hostname> [dns_server]\n", argv[0]);
        return 1;
    }

    const char *hostname = argv[1];
    const char *dns_server = (argc > 2) ? argv[2] : "8.8.8.8";

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(53);
    dest.sin_addr.s_addr = inet_addr(dns_server);

    unsigned char buf[512];
    memset(buf, 0, sizeof(buf));
    struct dns_header *dns = (struct dns_header *)buf;

    dns->id = (uint16_t) htons(getpid());
    dns->flags = htons(0x0100); // Standard query
    dns->qdcount = htons(1);
    dns->ancount = 0;
    dns->nscount = 0;
    dns->arcount = 0;

    unsigned char *qname = &buf[sizeof(struct dns_header)];
    char *hostname_copy = strdup(hostname);
    format_dns_name(qname, (unsigned char*)hostname_copy);

    unsigned char *qtype = qname + strlen((char*)qname) + 1;
    *((uint16_t*)qtype) = htons(1); // Type A
    *((uint16_t*)(qtype + 2)) = htons(1); // Class IN

    int query_len = sizeof(struct dns_header) + (strlen((char*)qname) + 1) + 4;

    printf("Resolving %s via %s...\n", hostname, dns_server);

    if (sendto(sock, buf, query_len, 0, (struct sockaddr*)&dest, sizeof(dest)) < 0) {
        perror("sendto");
        free(hostname_copy);
        close(sock);
        return 1;
    }

    socklen_t dest_len = sizeof(dest);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&dest, &dest_len);
    if (n < 0) {
        perror("recvfrom");
        free(hostname_copy);
        close(sock);
        return 1;
    }

    printf("Received %d bytes from DNS server\n", n);

    // Basic parsing (just skip question and find the first A record)
    dns = (struct dns_header *)buf;
    if (ntohs(dns->ancount) > 0) {
        // Skip header and question
        unsigned char *reader = &buf[query_len];
        // Now at the answer section
        // DNS Answer format: Name, Type(2), Class(2), TTL(4), RDLength(2), RData(...)
        
        // Skip Name (compressed or not)
        if ((*reader & 0xc0) == 0xc0) {
            reader += 2; // Pointer
        } else {
            while (*reader) reader++;
            reader++;
        }
        
        uint16_t type = ntohs(*(uint16_t*)reader);
        reader += 2;
        // uint16_t class = ntohs(*(uint16_t*)reader);
        reader += 2;
        // uint32_t ttl = ntohl(*(uint32_t*)reader);
        reader += 4;
        uint16_t rdlen = ntohs(*(uint16_t*)reader);
        reader += 2;
        
        if (type == 1 && rdlen == 4) {
            printf("IP Address: %d.%d.%d.%d\n", reader[0], reader[1], reader[2], reader[3]);
        } else {
            printf("Answer found but not a standard IPv4 address (Type %d, Len %d)\n", type, rdlen);
        }
    } else {
        printf("No answers found.\n");
    }

    free(hostname_copy);
    close(sock);
    return 0;
}
