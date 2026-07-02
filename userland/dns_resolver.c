#include "dns_resolver.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0]<<8|p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0]=v>>8; p[1]=(uint8_t)v; }

static int skip_name(const uint8_t *p, size_t n, size_t *off) {
    size_t pos=*off, hops=0;
    while (pos<n && hops++<128) {
        uint8_t l=p[pos++];
        if (!l) { *off=pos; return 0; }
        if ((l&0xc0)==0xc0) {
            if (pos>=n || (((size_t)(l&0x3f)<<8)|p[pos])>=n) return -1;
            *off=pos+1; return 0;
        }
        if ((l&0xc0) || l>63 || pos+l>n) return -1;
        pos+=l;
    }
    return -1;
}

int dns_build_query_type(uint8_t *out, size_t cap, uint16_t id,
                         const char *name, uint16_t type) {
    if (!out || !name || cap<17) return -1;
    memset(out,0,cap); put16(out,id); put16(out+2,0x0100); put16(out+4,1);
    size_t pos=12; const char *s=name;
    while (*s) {
        const char *dot=strchr(s,'.'); size_t l=dot?(size_t)(dot-s):strlen(s);
        if (!l || l>63 || pos+1+l+5>cap) return -1;
        out[pos++]=(uint8_t)l; memcpy(out+pos,s,l); pos+=l;
        if (!dot) break;
        s=dot+1;
    }
    out[pos++]=0; put16(out+pos,type); put16(out+pos+2,1);
    return (int)(pos+4);
}

int dns_build_query(uint8_t *out, size_t cap, uint16_t id, const char *name) {
    return dns_build_query_type(out,cap,id,name,1);
}

int dns_parse_a_reply(const uint8_t *p, size_t n, uint16_t id, uint32_t *addr) {
    if (!p || !addr || n<12 || get16(p)!=id || !(get16(p+2)&0x8000) ||
        (get16(p+2)&0x000f) || get16(p+4)!=1) return -1;
    size_t off=12;
    if (skip_name(p,n,&off)<0 || off+4>n) return -1;
    off+=4;
    uint16_t answers=get16(p+6);
    for (uint16_t i=0;i<answers;i++) {
        if (skip_name(p,n,&off)<0 || off+10>n) return -1;
        uint16_t type=get16(p+off), cls=get16(p+off+2), rdlen=get16(p+off+8);
        off+=10;
        if (off+rdlen>n) return -1;
        if (type==1 && cls==1 && rdlen==4) { memcpy(addr,p+off,4); return 0; }
        off+=rdlen; /* Includes CNAMEs; a following A record is accepted. */
    }
    return -1;
}

int dns_parse_aaaa_reply(const uint8_t *p, size_t n, uint16_t id,
                         uint8_t addr[16]) {
    if (!p || !addr || n<12 || get16(p)!=id || !(get16(p+2)&0x8000) ||
        (get16(p+2)&0x000f) || get16(p+4)!=1) return -1;
    size_t off=12;
    if (skip_name(p,n,&off)<0 || off+4>n) return -1;
    off+=4;
    uint16_t answers=get16(p+6);
    for (uint16_t i=0;i<answers;i++) {
        if (skip_name(p,n,&off)<0 || off+10>n) return -1;
        uint16_t type=get16(p+off), cls=get16(p+off+2), rdlen=get16(p+off+8);
        off+=10;
        if (off+rdlen>n) return -1;
        if (type==28 && cls==1 && rdlen==16) {
            memcpy(addr,p+off,16); return 0;
        }
        off+=rdlen;
    }
    return -1;
}

int dns_resolve_a(const struct dns_resolver *r, const char *name, uint32_t *addr) {
    if (!r || !r->server_count || !addr) { errno=EINVAL; return -1; }
    uint8_t query[512], reply[512];
    uint16_t id=(uint16_t)(getpid() ^ (uintptr_t)name);
    int qlen=dns_build_query(query,sizeof(query),id,name);
    if (qlen<0) { errno=EINVAL; return -1; }
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,IPPROTO_UDP);
    if (fd<0) return -1;
    int attempts=r->attempts>0?r->attempts:2;
    for (int a=0;a<attempts;a++) for (size_t i=0;i<r->server_count;i++) {
        struct sockaddr_in sa;
        memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET;
        sa.sin_port=htons(53); sa.sin_addr.s_addr=r->servers[i];
        if (sendto(fd,query,(size_t)qlen,0,(struct sockaddr*)&sa,sizeof(sa))<0)
            continue;
        struct pollfd pfd={.fd=fd,.events=POLLIN};
        if (poll(&pfd,1,r->timeout_ms>0?r->timeout_ms:1500)<=0) continue;
        ssize_t n=recv(fd,reply,sizeof(reply),0);
        if (n>0 && dns_parse_a_reply(reply,(size_t)n,id,addr)==0) {
            close(fd); return 0;
        }
    }
    close(fd); errno=ETIMEDOUT; return -1;
}

int dns_resolve_aaaa(const struct dns_resolver *r, const char *name,
                     uint8_t addr[16]) {
    if (!r || !r->server_count || !addr) { errno=EINVAL; return -1; }
    uint8_t query[512], reply[512];
    uint16_t id=(uint16_t)(getpid() ^ (uintptr_t)name ^ 0xaaaa);
    int qlen=dns_build_query_type(query,sizeof(query),id,name,28);
    if (qlen<0) { errno=EINVAL; return -1; }
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,IPPROTO_UDP);
    if (fd<0) return -1;
    int attempts=r->attempts>0?r->attempts:2;
    for (int a=0;a<attempts;a++) for (size_t i=0;i<r->server_count;i++) {
        struct sockaddr_in sa;
        memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET;
        sa.sin_port=htons(53); sa.sin_addr.s_addr=r->servers[i];
        if (sendto(fd,query,(size_t)qlen,0,(struct sockaddr*)&sa,sizeof(sa))<0)
            continue;
        struct pollfd pfd={.fd=fd,.events=POLLIN};
        if (poll(&pfd,1,r->timeout_ms>0?r->timeout_ms:1500)<=0) continue;
        ssize_t n=recv(fd,reply,sizeof(reply),0);
        if (n>0 && dns_parse_aaaa_reply(reply,(size_t)n,id,addr)==0) {
            close(fd); return 0;
        }
    }
    close(fd); errno=ETIMEDOUT; return -1;
}
