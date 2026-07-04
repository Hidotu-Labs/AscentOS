#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include <tinygl/GL/gl.h>
#include <tinygl/zbuffer.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#ifndef M_PI
#define M_PI 3.14159265
#endif

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield {
  uint32_t offset;
  uint32_t length;
  uint32_t msb_right;
};
struct fb_var_screeninfo {
  uint32_t xres;
  uint32_t yres;
  uint32_t xres_virtual;
  uint32_t yres_virtual;
  uint32_t xoffset;
  uint32_t yoffset;
  uint32_t bits_per_pixel;
  uint32_t grayscale;
  struct fb_bitfield red;
  struct fb_bitfield green;
  struct fb_bitfield blue;
  struct fb_bitfield transp;
  uint32_t nonstd;
  uint32_t activate;
  uint32_t height;
  uint32_t width;
  uint32_t accel_flags;
  uint32_t pixclock;
  uint32_t left_margin;
  uint32_t right_margin;
  uint32_t upper_margin;
  uint32_t lower_margin;
  uint32_t hsync_len;
  uint32_t vsync_len;
  uint32_t sync;
  uint32_t vmode;
  uint32_t rotate;
  uint32_t colorspace;
  uint32_t reserved[4];
};
struct fb_fix_screeninfo {
  char id[16];
  unsigned long smem_start;
  uint32_t smem_len;
  uint32_t type;
  uint32_t type_aux;
  uint32_t visual;
  uint16_t xpanstep;
  uint16_t ypanstep;
  uint16_t ywrapstep;
  uint32_t line_length;
  unsigned long mmio_start;
  uint32_t mmio_len;
  uint32_t accel;
  uint16_t capabilities;
  uint16_t reserved[2];
};

int override_drawmodes = 0;

static void gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
                 GLint teeth, GLfloat tooth_depth) {
  GLint i;
  GLfloat r0, r1, r2, angle, da, u, v, len;

  r0 = inner_radius;
  r1 = outer_radius - tooth_depth / 2.0;
  r2 = outer_radius + tooth_depth / 2.0;
  da = 2.0 * M_PI / teeth / 4.0;

  glNormal3f(0.0, 0.0, 1.0);
  glBegin(GL_QUAD_STRIP);
  for (i = 0; i <= teeth; i++) {
    angle = i * 2.0 * M_PI / teeth;
    glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
    glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
    glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
    glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), width * 0.5);
  }
  glEnd();

  glBegin(GL_QUADS);
  da = 2.0 * M_PI / teeth / 4.0;
  for (i = 0; i < teeth; i++) {
    angle = i * 2.0 * M_PI / teeth;
    glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
    glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5);
    glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), width * 0.5);
    glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), width * 0.5);
  }
  glEnd();

  glNormal3f(0.0, 0.0, -1.0);
  glBegin(GL_QUAD_STRIP);
  for (i = 0; i <= teeth; i++) {
    angle = i * 2.0 * M_PI / teeth;
    glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
    glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
    glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
               -width * 0.5);
    glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
  }
  glEnd();

  glBegin(GL_QUADS);
  da = 2.0 * M_PI / teeth / 4.0;
  for (i = 0; i < teeth; i++) {
    angle = i * 2.0 * M_PI / teeth;
    glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
               -width * 0.5);
    glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
               -width * 0.5);
    glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5);
    glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
  }
  glEnd();

  glBegin(GL_QUAD_STRIP);
  for (i = 0; i < teeth; i++) {
    angle = i * 2.0 * M_PI / teeth;
    glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
    glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
    u = r2 * cos(angle + da) - r1 * cos(angle);
    v = r2 * sin(angle + da) - r1 * sin(angle);
    len = sqrt(u * u + v * v);
    u /= len;
    v /= len;
    glNormal3f(v, -u, 0.0);
    glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5);
    glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5);
    glNormal3f(cos(angle), sin(angle), 0.0);
    glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da), width * 0.5);
    glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
               -width * 0.5);
    u = r1 * cos(angle + 3 * da) - r2 * cos(angle + 2 * da);
    v = r1 * sin(angle + 3 * da) - r2 * sin(angle + 2 * da);
    glNormal3f(v, -u, 0.0);
    glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da), width * 0.5);
    glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
               -width * 0.5);
    glNormal3f(cos(angle), sin(angle), 0.0);
  }
  glVertex3f(r1 * cos(0), r1 * sin(0), width * 0.5);
  glVertex3f(r1 * cos(0), r1 * sin(0), -width * 0.5);
  glEnd();

  glBegin(GL_QUAD_STRIP);
  for (i = 0; i <= teeth; i++) {
    angle = i * 2.0 * M_PI / teeth;
    glNormal3f(-cos(angle), -sin(angle), 0.0);
    glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
    glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
  }
  glEnd();
}

static GLfloat view_rotx = 20.0, view_roty = 30.0;
static GLint gear1, gear2, gear3;
static GLfloat angle = 0.0;

void draw() {
  angle += 2.0;
  glPushMatrix();
  glRotatef(view_rotx, 1.0, 0.0, 0.0);
  glRotatef(view_roty, 0.0, 1.0, 0.0);

  glPushMatrix();
  glTranslatef(-3.0, -2.0, 0.0);
  glRotatef(angle, 0.0, 0.0, 1.0);
  glCallList(gear1);
  glPopMatrix();

  glPushMatrix();
  glTranslatef(3.1, -2.0, 0.0);
  glRotatef(-2.0 * angle - 9.0, 0.0, 0.0, 1.0);
  glCallList(gear2);
  glPopMatrix();

  glPushMatrix();
  glTranslatef(-3.1, 4.2, 0.0);
  glRotatef(-2.0 * angle - 25.0, 0.0, 0.0, 1.0);
  glCallList(gear3);
  glPopMatrix();

  glPopMatrix();
}

void initScene() {
  static GLfloat pos[4] = {5, 5, 10, 0.0};
  static GLfloat red[4] = {1.0, 0.0, 0.0, 0.0};
  static GLfloat green[4] = {0.0, 1.0, 0.0, 0.0};
  static GLfloat blue[4] = {0.0, 0.0, 1.0, 0.0};
  static GLfloat white[4] = {1.0, 1.0, 1.0, 0.0};
  static GLfloat shininess = 5;

  glLightfv(GL_LIGHT0, GL_POSITION, pos);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
  glLightfv(GL_LIGHT0, GL_SPECULAR, white);
  glEnable(GL_CULL_FACE);
  glEnable(GL_LIGHT0);

  gear1 = glGenLists(1);
  glNewList(gear1, GL_COMPILE);
  glMaterialfv(GL_FRONT, GL_DIFFUSE, blue);
  glMaterialfv(GL_FRONT, GL_SPECULAR, white);
  glMaterialfv(GL_FRONT, GL_SHININESS, &shininess);
  glColor3fv(blue);
  gear(1.0, 4.0, 1.0, 20, 0.7);
  glEndList();

  gear2 = glGenLists(1);
  glNewList(gear2, GL_COMPILE);
  glMaterialfv(GL_FRONT, GL_DIFFUSE, red);
  glMaterialfv(GL_FRONT, GL_SPECULAR, white);
  glColor3fv(red);
  gear(0.5, 2.0, 2.0, 10, 0.7);
  glEndList();

  gear3 = glGenLists(1);
  glNewList(gear3, GL_COMPILE);
  glMaterialfv(GL_FRONT, GL_DIFFUSE, green);
  glMaterialfv(GL_FRONT, GL_SPECULAR, white);
  glColor3fv(green);
  gear(1.3, 2.0, 0.5, 10, 0.7);
  glEndList();
}

int main(int argc, char **argv) {
  // Never seize the physical framebuffer merely because X connection failed.
  // Bare-metal rendering must be explicitly requested with -fb.
  int use_x11 = 1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-fb") == 0)
      use_x11 = 0;
    if (strcmp(argv[i], "-x11") == 0)
      use_x11 = 1;
  }

  uint32_t width = 0, height = 0;
  int fd = -1;
  uint32_t *fb = NULL;
  uint32_t line_length = 0;

  Display *dpy = NULL;
  Window win = 0;
  GC gc = 0;
  XImage *img = NULL;

  if (use_x11) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
      fprintf(stderr,
              "Cannot open X display. Set DISPLAY or use -fb explicitly.\n");
      return 1;
    }
  }

  if (use_x11) {
    int screen = DefaultScreen(dpy);
    width = 320;
    height = 200;
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 10, 10, width,
                              height, 1, BlackPixel(dpy, screen),
                              BlackPixel(dpy, screen));
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(dpy, win);
    XStoreName(dpy, win, "TinyGL Gears (X11 Backend)");
    gc = XCreateGC(dpy, win, 0, NULL);

    fb = malloc(width * height * 4);
    line_length = width * 4;
    img =
        XCreateImage(dpy, DefaultVisual(dpy, screen), DefaultDepth(dpy, screen),
                     ZPixmap, 0, (char *)fb, width, height, 32, 0);
    printf("Starting TinyGL X11 renderer (%ux%u)...\n", width, height);
  } else {
    fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
      perror("open /dev/fb0 failed");
      return 1;
    }
    struct fb_var_screeninfo var;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
      perror("FBIOGET_VSCREENINFO");
      return 1;
    }
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
      perror("FBIOGET_FSCREENINFO");
      return 1;
    }

    width = var.xres;
    height = var.yres;
    line_length = fix.line_length;
    fb = (uint32_t *)mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
      perror("mmap");
      return 1;
    }
    printf("Starting TinyGL bare-metal framebuffer renderer (%ux%u)...\n",
           width, height);
  }

  // Initialize TinyGL with the active resolution!
  ZBuffer *frameBuffer = ZB_open(width, height, ZB_MODE_RGBA, 0);
  if (!frameBuffer) {
    printf("ZB_open failed!\n");
    return 1;
  }
  glInit(frameBuffer);

  glClearColor(0.0, 0.0, 0.0, 0.0);
  glViewport(0, 0, width, height);
  glShadeModel(GL_SMOOTH);
  glEnable(GL_LIGHTING);
  glEnable(GL_DEPTH_TEST);

  GLfloat h = (GLfloat)height / (GLfloat)width;
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glFrustum(-1.0, 1.0, -h, h, 5.0, 60.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glTranslatef(0.0, 0.0, -45.0);

  initScene();

  struct timeval tv;
  gettimeofday(&tv, NULL);
  long start_time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
  int frames = 0;

  // Loop forever rendering
  while (1) {
    if (use_x11) {
      while (XPending(dpy)) {
        XEvent event;
        XNextEvent(dpy, &event);
        if (event.type == KeyPress)
          goto end_loop;
        if (event.type == ConfigureNotify) {
          XConfigureEvent *ce = &event.xconfigure;
          if (ce->width != width || ce->height != height) {
            width = ce->width;
            height = ce->height;
            line_length = width * 4;

            if (img) {
              img->data = NULL; // prevent XDestroyImage from freeing fb yet
              XDestroyImage(img);
            }
            if (fb)
              free(fb);

            fb = malloc(width * height * 4);
            img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                               DefaultDepth(dpy, DefaultScreen(dpy)), ZPixmap,
                               0, (char *)fb, width, height, 32, 0);

            glClose();
            ZB_close(frameBuffer);

            frameBuffer = ZB_open(width, height, ZB_MODE_RGBA, 0);
            glInit(frameBuffer);

            glClearColor(0.0, 0.0, 0.0, 0.0);
            glViewport(0, 0, width, height);
            glShadeModel(GL_SMOOTH);
            glEnable(GL_LIGHTING);
            glEnable(GL_DEPTH_TEST);

            GLfloat h = (GLfloat)height / (GLfloat)width;
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glFrustum(-1.0, 1.0, -h, h, 5.0, 60.0);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glTranslatef(0.0, 0.0, -45.0);

            initScene();
          }
        }
      }
    }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw();

    // ZB_copyFrameBuffer copies internal pixel buffer to the target memory
    ZB_copyFrameBuffer(frameBuffer, fb, line_length);

    if (use_x11) {
      XPutImage(dpy, win, gc, img, 0, 0, 0, 0, width, height);
      XSync(dpy, False);
    }

    frames++;
    gettimeofday(&tv, NULL);
    long current_time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    if (current_time - start_time >= 5000) {
      float fps = frames / 5.0f;
      printf("%d frames in 5.0 seconds = %.3f FPS\n", frames, fps);
      frames = 0;
      start_time = current_time;
    }
  }
end_loop:

  ZB_close(frameBuffer);
  glClose();

  if (use_x11) {
    XDestroyImage(img);
    XCloseDisplay(dpy);
  } else {
    close(fd);
  }

  return 0;
}
