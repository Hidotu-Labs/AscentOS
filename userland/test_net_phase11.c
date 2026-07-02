#include "dns_resolver.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int passed, failed;
#define T(x,n) do { if (x) { printf("[PASS] %s\n",n); passed++; } else { \
  printf("[FAIL] %s: %s\n",n,strerror(errno)); failed++; } } while (0)

static void synthetic_aaaa(uint8_t *p, size_t *length, uint16_t id) {
  memset(p,0,64); p[0]=id>>8; p[1]=id; p[2]=0x81; p[3]=0x80;
  p[5]=1; p[7]=1; size_t n=12;
  p[n++]=1; p[n++]='x'; p[n++]=0; p[n++]=0; p[n++]=28; p[n++]=0; p[n++]=1;
  p[n++]=0xc0; p[n++]=0x0c; p[n++]=0; p[n++]=28; p[n++]=0; p[n++]=1;
  p[n++]=0; p[n++]=0; p[n++]=0; p[n++]=60; p[n++]=0; p[n++]=16;
  const uint8_t a[16]={0x20,1,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1};
  memcpy(p+n,a,16); n+=16; *length=n;
}

int main(int argc,char **argv) {
  int fd=socket(AF_INET6,SOCK_DGRAM,IPPROTO_UDP);
  T(fd>=0,"AF_INET6 UDP socket");
  struct sockaddr_in6 any, name; socklen_t namelen=sizeof(name);
  memset(&any,0,sizeof(any)); any.sin6_family=AF_INET6;
  T(fd>=0&&bind(fd,(struct sockaddr*)&any,sizeof(any))==0,"UDPv6 wildcard bind");
  T(fd>=0&&getsockname(fd,(struct sockaddr*)&name,&namelen)==0&&
    name.sin6_family==AF_INET6&&name.sin6_port,"UDPv6 getsockname");
  T(fd>=0&&fcntl(fd,F_SETFL,O_NONBLOCK)==0,"UDPv6 fcntl nonblocking");
  char b; errno=0;
  T(fd>=0&&recv(fd,&b,1,0)<0&&(errno==EAGAIN||errno==EWOULDBLOCK),
    "UDPv6 nonblocking receive");
  struct pollfd pfd={.fd=fd,.events=POLLOUT};
  T(fd>=0&&poll(&pfd,1,0)==1&&(pfd.revents&POLLOUT),"UDPv6 poll writable");
  if(fd>=0)close(fd);

  fd=socket(AF_INET6,SOCK_STREAM,IPPROTO_TCP);
  T(fd>=0,"AF_INET6 TCP socket");
  T(fd>=0&&bind(fd,(struct sockaddr*)&any,sizeof(any))==0&&listen(fd,4)==0,
    "TCPv6 bind and listen");
  if(fd>=0) fcntl(fd,F_SETFL,O_NONBLOCK);
  errno=0;
  T(fd>=0&&accept(fd,NULL,NULL)<0&&(errno==EAGAIN||errno==EWOULDBLOCK),
    "TCPv6 nonblocking accept");
  if(fd>=0)close(fd);

  uint8_t query[128]; int q=dns_build_query_type(query,sizeof(query),0x1234,"x",28);
  T(q>4&&query[q-4]==0&&query[q-3]==28,"DNS AAAA query construction");
  uint8_t reply[64],address[16],expected[16]={0x20,1,0x0d,0xb8}; size_t rlen;
  expected[15]=1; synthetic_aaaa(reply,&rlen,0x1234);
  T(dns_parse_aaaa_reply(reply,rlen,0x1234,address)==0&&
    !memcmp(address,expected,16),"DNS AAAA compressed reply parser");

  if(argc>1) {
    struct dns_resolver r={.servers={inet_addr("10.0.2.3")},.server_count=1,
                           .timeout_ms=2000,.attempts=2};
    T(dns_resolve_aaaa(&r,argv[1],address)==0,"live DNS AAAA resolution");
  }

  if(argc>3) {
    struct sockaddr_in6 peer; memset(&peer,0,sizeof(peer)); peer.sin6_family=AF_INET6;
    inet_pton(AF_INET6,argv[2],&peer.sin6_addr);
    const char msg[]="phase11-v6";
    peer.sin6_port=htons((uint16_t)atoi(argv[3]));
    fd=socket(AF_INET6,SOCK_DGRAM,IPPROTO_UDP);
    int ok=fd>=0&&sendto(fd,msg,sizeof(msg),0,(struct sockaddr*)&peer,sizeof(peer))==
      (ssize_t)sizeof(msg);
    struct pollfd wait={.fd=fd,.events=POLLIN}; char echo[32]={0};
    ok=ok&&poll(&wait,1,3000)==1&&recv(fd,echo,sizeof(echo),0)==(ssize_t)sizeof(msg)&&
       !memcmp(echo,msg,sizeof(msg));
    T(ok,"UDPv6 host echo");
    if(fd>=0)close(fd);
  }

  if(argc>4) {
    struct sockaddr_in6 peer; memset(&peer,0,sizeof(peer)); peer.sin6_family=AF_INET6;
    inet_pton(AF_INET6,argv[2],&peer.sin6_addr); peer.sin6_port=htons((uint16_t)atoi(argv[4]));
    fd=socket(AF_INET6,SOCK_STREAM,IPPROTO_TCP);
    const char msg[]="phase11-tcp6"; char echo[32]={0};
    int ok=fd>=0&&connect(fd,(struct sockaddr*)&peer,sizeof(peer))==0&&
           send(fd,msg,sizeof(msg),0)==(ssize_t)sizeof(msg)&&
           recv(fd,echo,sizeof(echo),0)==(ssize_t)sizeof(msg)&&
           !memcmp(echo,msg,sizeof(msg));
    T(ok,"TCPv6 host echo");
    if(fd>=0)close(fd);
  }

  printf("[NET TEST] Phase 11 %s: %d passed, %d failed\n",
         failed?"FAIL":"PASS",passed,failed);
  return failed?1:0;
}
