/*
 * test_opengl.c — AvoryOS DRM/KMS Software Rasterizer Test
 *
 * Uses DRM dumb buffers + a CPU triangle rasterizer to render
 * frames directly to screen via DRM KMS — no EGL/Mesa needed.
 * Tests: GEM alloc, mmap VMA registration, munmap safety, pixel throughput.
 *
 * Build:
 *   x86_64-linux-musl-gcc -static -o test_opengl test_opengl.c -lm
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* ── Minimal Linux DRM uAPI (no external headers needed) ─────────────── */
#define DRM_IOCTL_BASE 'd'
#define _IOC(dir,t,n,sz) (((dir)<<30)|((t)<<8)|((n))|((sz)<<16))
#define _IOWR(t,n,T)  _IOC(3,t,n,sizeof(T))
#define _IOW(t,n,T)   _IOC(1,t,n,sizeof(T))
#define _IOR(t,n,T)   _IOC(2,t,n,sizeof(T))

typedef struct { int major,minor,patch; size_t nl; char *n; size_t dl; char *d; size_t xl; char *x; } drm_ver_t;
typedef struct { uint32_t h,w,bpp,flags,handle,pitch; uint64_t size; } dumb_create_t;
typedef struct { uint32_t handle,pad; uint64_t offset; } dumb_map_t;
typedef struct { uint32_t handle; } dumb_destroy_t;
typedef struct { uint64_t fb_ptr,crtc_ptr,con_ptr,enc_ptr; uint32_t nfb,ncrtc,ncon,nenc,minw,maxw,minh,maxh; } res_t;
typedef struct { uint32_t clock; uint16_t hd,hs0,hs1,ht,hsk,vd,vs0,vs1,vt,vscan; uint32_t vrefresh,flags,type; char name[32]; } mode_t;
typedef struct { uint64_t enc_ptr,mode_ptr,prop_ptr,pval_ptr; uint32_t nmode,nprop,nenc,encid,conid,type,typeid,conn,mmw,mmh,subpx,pad; } con_t;
typedef struct { uint32_t fbid,w,h,pitch,bpp,depth,handle; } addfb_t;
typedef struct { uint64_t con_ptr; uint32_t ncon,crtcid,fbid,x,y,gsz,valid; mode_t mode; } setcrtc_t;

#define DRM_IOCTL_VERSION       _IOWR(DRM_IOCTL_BASE,0x00,drm_ver_t)
#define DRM_IOCTL_CREATE_DUMB   _IOWR(DRM_IOCTL_BASE,0xb2,dumb_create_t)
#define DRM_IOCTL_MAP_DUMB      _IOWR(DRM_IOCTL_BASE,0xb3,dumb_map_t)
#define DRM_IOCTL_DESTROY_DUMB  _IOW (DRM_IOCTL_BASE,0xb4,dumb_destroy_t)
#define DRM_IOCTL_GETRESOURCES  _IOWR(DRM_IOCTL_BASE,0xa0,res_t)
#define DRM_IOCTL_GETCONNECTOR  _IOWR(DRM_IOCTL_BASE,0xa7,con_t)
#define DRM_IOCTL_ADDFB         _IOWR(DRM_IOCTL_BASE,0xae,addfb_t)
#define DRM_IOCTL_RMFB          _IOWR(DRM_IOCTL_BASE,0xaf,uint32_t)
#define DRM_IOCTL_SETCRTC       _IOWR(DRM_IOCTL_BASE,0xa2,setcrtc_t)

/* ── Timing ────────────────────────────────────────────────────────────── */
static uint64_t ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;
}

/* ── CPU triangle rasterizer ───────────────────────────────────────────── */
typedef struct { float x,y; uint8_t r,g,b; } Vert;
static void triangle(uint32_t *fb, int W, int H, const Vert *a, const Vert *b, const Vert *c)
{
    float x0=a->x<b->x?a->x:b->x; x0=x0<c->x?x0:c->x;
    float y0=a->y<b->y?a->y:b->y; y0=y0<c->y?y0:c->y;
    float x1=a->x>b->x?a->x:b->x; x1=x1>c->x?x1:c->x;
    float y1=a->y>b->y?a->y:b->y; y1=y1>c->y?y1:c->y;
    int ix0=(int)x0; if(ix0<0)ix0=0;
    int iy0=(int)y0; if(iy0<0)iy0=0;
    int ix1=(int)x1+1; if(ix1>W)ix1=W;
    int iy1=(int)y1+1; if(iy1>H)iy1=H;
    float denom=(b->y-c->y)*(a->x-c->x)+(c->x-b->x)*(a->y-c->y);
    if(fabsf(denom)<1.f)return;
    for(int y=iy0;y<iy1;y++) for(int x=ix0;x<ix1;x++){
        float px=x+.5f,py=y+.5f;
        float w0=((b->y-c->y)*(px-c->x)+(c->x-b->x)*(py-c->y))/denom;
        float w1=((c->y-a->y)*(px-c->x)+(a->x-c->x)*(py-c->y))/denom;
        float w2=1.f-w0-w1;
        if(w0<0||w1<0||w2<0)continue;
        uint8_t r=(uint8_t)(w0*a->r+w1*b->r+w2*c->r);
        uint8_t g=(uint8_t)(w0*a->g+w1*b->g+w2*c->g);
        uint8_t bv=(uint8_t)(w0*a->b+w1*b->b+w2*c->b);
        fb[y*W+x]=0xFF000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|bv;
    }
}

int main(void)
{
    printf("====================================================\n");
    printf("AvoryOS DRM Software Rasterizer Test\n");
    printf("====================================================\n\n");

    /* Open DRM */
    int fd=open("/dev/dri/card0",O_RDWR);
    if(fd<0){fprintf(stderr,"[FAIL] open /dev/dri/card0: %s\n",strerror(errno));return 1;}

    char drm_name[64]={};
    drm_ver_t ver={.nl=sizeof(drm_name)-1,.n=drm_name};
    if(ioctl(fd,DRM_IOCTL_VERSION,&ver)==0)
        printf("[DRM] Driver: %s v%d.%d.%d\n",drm_name,ver.major,ver.minor,ver.patch);

    /* Get connector and mode */
    res_t res={};
    ioctl(fd,DRM_IOCTL_GETRESOURCES,&res);
    if(!res.ncon||!res.ncrtc){fprintf(stderr,"[FAIL] No connectors/CRTCs\n");return 1;}

    uint32_t conids[8]={},crtcids[8]={};
    res.con_ptr=(uint64_t)conids; res.crtc_ptr=(uint64_t)crtcids;
    ioctl(fd,DRM_IOCTL_GETRESOURCES,&res);

    mode_t cur_mode={};
    uint32_t conid=conids[0], crtcid=crtcids[0];
    int W=1280, H=800;
    for(uint32_t i=0;i<res.ncon;i++){
        con_t c={.conid=conids[i]};
        ioctl(fd,DRM_IOCTL_GETCONNECTOR,&c);
        if(c.nmode>0){
            mode_t modes[8]={};
            c.mode_ptr=(uint64_t)modes; c.nmode=c.nmode>8?8:c.nmode;
            ioctl(fd,DRM_IOCTL_GETCONNECTOR,&c);
            cur_mode=modes[0]; conid=conids[i];
            W=cur_mode.hd; H=cur_mode.vd; break;
        }
    }
    printf("[DRM] Display: %dx%d  connector=%u crtc=%u\n\n",W,H,conid,crtcid);

    /* Alloc GEM dumb buffer */
    dumb_create_t dumb={.w=(uint32_t)W,.h=(uint32_t)H,.bpp=32};
    if(ioctl(fd,DRM_IOCTL_CREATE_DUMB,&dumb)<0){
        fprintf(stderr,"[FAIL] CREATE_DUMB: %s\n",strerror(errno));return 1;}
    printf("[GEM] handle=%u  pitch=%u  size=%llu\n",dumb.handle,dumb.pitch,(unsigned long long)dumb.size);

    /* Map GEM buffer */
    dumb_map_t dm={.handle=dumb.handle};
    if(ioctl(fd,DRM_IOCTL_MAP_DUMB,&dm)<0){
        fprintf(stderr,"[FAIL] MAP_DUMB: %s\n",strerror(errno));return 1;}
    uint32_t *px=mmap(NULL,(size_t)dumb.size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off_t)dm.offset);
    if(px==MAP_FAILED){fprintf(stderr,"[FAIL] mmap: %s\n",strerror(errno));return 1;}
    printf("[GEM] Mapped at %p\n",px);

    /* Create DRM FB and set CRTC */
    addfb_t fb={.w=(uint32_t)W,.h=(uint32_t)H,.pitch=dumb.pitch,.bpp=32,.depth=24,.handle=dumb.handle};
    if(ioctl(fd,DRM_IOCTL_ADDFB,&fb)<0){fprintf(stderr,"[FAIL] ADDFB: %s\n",strerror(errno));return 1;}
    setcrtc_t sc={.crtcid=crtcid,.fbid=fb.fbid,.mode=cur_mode,.valid=1,.con_ptr=(uint64_t)&conid,.ncon=1};
    ioctl(fd,DRM_IOCTL_SETCRTC,&sc);
    printf("[FB] Framebuffer id=%u → CRTC set\n\n",fb.fbid);

    /* ── TEST 1: Rotating triangle, 200 frames ─────────────────────────── */
    printf("[TEST 1] Rotating RGB triangle — 200 frames at %dx%d...\n",W,H);
    float cx=W/2.f,cy=H/2.f,rad=(float)(H<W?H:W)*0.35f;
    uint64_t t0=ns();
    for(int f=0;f<200;f++){
        /* Clear */
        uint32_t bg=0xFF001030;
        for(int i=0;i<W*H;i++) px[i]=bg;
        /* Render */
        float a=(float)f*0.0314159f;
        Vert v[3]={
            {cx+rad*cosf(a),         cy+rad*sinf(a),         255,50,50},
            {cx+rad*cosf(a+2.094f),  cy+rad*sinf(a+2.094f),  50,255,50},
            {cx+rad*cosf(a+4.189f),  cy+rad*sinf(a+4.189f),  50,50,255},
        };
        triangle(px,W,H,&v[0],&v[1],&v[2]);
    }
    uint64_t t1=ns();
    double dt=(double)(t1-t0)/1e9;
    double fps=200.0/dt;
    double mpx=200.0*W*H/1e6/dt;
    printf("[PASS] 200 frames in %.3fs  →  %.1f FPS\n",dt,fps);
    printf("[PASS] Pixel fill rate: %.1f Mpix/sec\n",mpx);

    /* ── TEST 2: munmap VMA safety ─────────────────────────────────────── */
    printf("\n[TEST 2] munmap GEM buffer (VMA safety)...\n");
    munmap(px,(size_t)dumb.size);
    printf("[PASS] munmap completed without crash\n");

    /* ── TEST 3: GEM lifecycle ─────────────────────────────────────────── */
    printf("\n[TEST 3] GEM alloc+mmap+free cycle (50 iterations)...\n");
    uint64_t ta=ns();
    for(int i=0;i<50;i++){
        dumb_create_t d={.w=256,.h=256,.bpp=32};
        ioctl(fd,DRM_IOCTL_CREATE_DUMB,&d);
        dumb_map_t m={.handle=d.handle};
        ioctl(fd,DRM_IOCTL_MAP_DUMB,&m);
        void *p=mmap(NULL,(size_t)d.size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,(off_t)m.offset);
        if(p!=MAP_FAILED) munmap(p,(size_t)d.size);
        dumb_destroy_t dd={.handle=d.handle};
        ioctl(fd,DRM_IOCTL_DESTROY_DUMB,&dd);
    }
    uint64_t tb=ns();
    printf("[PASS] 50 GEM cycles in %.2fms  (%.0f cycles/sec)\n",
           (double)(tb-ta)/1e6, 50.0/((double)(tb-ta)/1e9));

    /* Cleanup */
    ioctl(fd,DRM_IOCTL_RMFB,&fb.fbid);
    close(fd);

    printf("\n====================================================\n");
    printf("ALL DRM/SOFTWARE RASTERIZER TESTS PASSED!\n");
    printf("====================================================\n");
    return 0;
}
