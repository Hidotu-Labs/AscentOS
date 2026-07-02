#include "dns_resolver.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static int pass, fail;
#define T(x,n) do { if(x){printf("[PASS] %s\n",n);pass++;}else{printf("[FAIL] %s\n",n);fail++;} } while(0)

int main(int argc, char **argv) {
    uint8_t q[512], r[512]; uint32_t ip=0; uint16_t id=0x7137;
    int qn=dns_build_query(q,sizeof(q),id,"example.com");
    T(qn>0 && q[12]==7 && !memcmp(q+13,"example",7),"bounded query encoding");
    memset(r,0,sizeof(r)); r[0]=id>>8;r[1]=id;r[2]=0x81;r[3]=0x80;
    r[5]=1;r[7]=1; memcpy(r+12,q+12,(size_t)qn-12);
    size_t o=(size_t)qn; r[o++]=0xc0;r[o++]=0x0c;r[o++]=0;r[o++]=1;
    r[o++]=0;r[o++]=1; o+=4;r[o++]=0;r[o++]=4;
    r[o++]=1;r[o++]=2;r[o++]=3;r[o++]=4;
    T(dns_parse_a_reply(r,o,id,&ip)==0 && ntohl(ip)==0x01020304,
      "compressed-name A reply");
    T(dns_parse_a_reply(r,o,id+1,&ip)<0,"transaction mismatch rejected");
    r[o-1]=0; T(dns_parse_a_reply(r,o-2,id,&ip)<0,"truncated RDATA rejected");
    r[12]=0xc0;r[13]=0x0c;
    T(dns_parse_a_reply(r,o,id,&ip)<0,"compression loop safely rejected");
    T(dns_build_query(q,20,id,"label-that-does-not-fit.example")<0,
      "query capacity enforced");
    if (argc>1) {
        struct dns_resolver d={.server_count=1,.timeout_ms=2000,.attempts=2};
        inet_pton(AF_INET,argc>2?argv[2]:"10.0.2.3",&d.servers[0]);
        T(dns_resolve_a(&d,argv[1],&ip)==0,"live UDP DNS lookup");
    } else printf("[SKIP] live lookup (pass hostname to enable)\n");
    printf("[NET TEST] Phase 7 %s: %d passed, %d failed\n",fail?"FAIL":"PASS",pass,fail);
    return fail?1:0;
}
