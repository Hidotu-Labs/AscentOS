#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int pass, fail;
#define T(x,n) do { if(x){printf("[PASS] %s\n",n);pass++;}else{printf("[FAIL] %s: %s\n",n,strerror(errno));fail++;} }while(0)

static struct sockaddr_in peer(const char *ip, unsigned port) {
  struct sockaddr_in a; memset(&a,0,sizeof(a)); a.sin_family=AF_INET;
  a.sin_port=htons((unsigned short)port);
  if(inet_pton(AF_INET,ip,&a.sin_addr)!=1) exit(2);
  return a;
}

int main(int argc,char **argv) {
  const char *host=argc>1?argv[1]:"10.0.2.2";
  unsigned port=argc>2?(unsigned)strtoul(argv[2],0,10):9001;
  struct sockaddr_in dst=peer(host,port), local, remote; socklen_t alen;
  int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
  T(fd>=0,"AF_INET TCP socket");
  if(fd<0) goto done;
  T(connect(fd,(struct sockaddr*)&dst,sizeof(dst))==0,"blocking connect");
  alen=sizeof(local);
  T(getsockname(fd,(struct sockaddr*)&local,&alen)==0&&local.sin_port,
    "getsockname after connect");
  alen=sizeof(remote);
  T(getpeername(fd,(struct sockaddr*)&remote,&alen)==0&&
    remote.sin_port==dst.sin_port,"getpeername after connect");
  const char msg[]="phase9-stream"; char buf[64]={0};
  T(send(fd,msg,sizeof(msg),0)==(ssize_t)sizeof(msg),"stream send");
  T(recv(fd,buf,sizeof(buf),0)==(ssize_t)sizeof(msg)&&
    !memcmp(buf,msg,sizeof(msg)),"stream receive");
  close(fd);

  fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK,IPPROTO_TCP);
  T(fd>=0,"nonblocking TCP socket");
  if(fd>=0) {
    int r=connect(fd,(struct sockaddr*)&dst,sizeof(dst));
    T(r==0||(r<0&&errno==EINPROGRESS),"nonblocking connect starts");
    struct pollfd p={.fd=fd,.events=POLLOUT};
    T(poll(&p,1,5000)==1&&(p.revents&(POLLOUT|POLLERR)),
      "nonblocking connect poll completion");
    close(fd);
  }
done:
  printf("[NET TEST] Phase 9 %s: %d passed, %d failed\n",
         fail?"FAIL":"PASS",pass,fail);
  return fail?1:0;
}
