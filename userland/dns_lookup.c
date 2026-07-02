#include "dns_resolver.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc<2) { fprintf(stderr,"usage: dns_lookup NAME [DNS...]\n"); return 2; }
    struct dns_resolver r={.timeout_ms=1500,.attempts=2};
    const char *fallback="10.0.2.3";
    for (int i=2;i<argc && r.server_count<DNS_MAX_SERVERS;i++)
        if (inet_pton(AF_INET,argv[i],&r.servers[r.server_count])==1)
            r.server_count++;
    if (!r.server_count) {
        inet_pton(AF_INET,fallback,&r.servers[0]); r.server_count=1;
    }
    uint32_t ip; char text[INET_ADDRSTRLEN];
    if (dns_resolve_a(&r,argv[1],&ip)<0) { perror("dns_lookup"); return 1; }
    printf("%s\n",inet_ntop(AF_INET,&ip,text,sizeof(text)));
    return 0;
}
