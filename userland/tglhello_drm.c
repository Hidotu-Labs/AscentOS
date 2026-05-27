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

#ifndef M_PI
#define M_PI 3.14159265
#endif

// DRM Definitions (simplified for userland)
#define DRM_IOCTL_VERSION          0xC0406400
#define DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0
#define DRM_IOCTL_MODE_CREATE_DUMB  0xC02064B2
#define DRM_IOCTL_MODE_MAP_DUMB     0xC01064B3

struct drm_version {
    int version_major, version_minor, version_patchlevel;
    size_t name_len; char *name;
    size_t date_len; char *date;
    size_t desc_len; char *desc;
};

struct drm_mode_map_dumb {
    uint32_t handle, pad;
    uint64_t offset;
};

static double time_passed = 0.0;

void draw() {
    time_passed += 0.0166666;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glPushMatrix();
    glTranslatef(0.0f, 0.0f, -6.0f);
    glRotatef(time_passed * 50.0, 1.0, 1.0, 0.5);
    
    glBegin(GL_QUADS);
    
    // Front Face
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f(-1.0f, -1.0f,  1.0f);
    glVertex3f( 1.0f, -1.0f,  1.0f);
    glVertex3f( 1.0f,  1.0f,  1.0f);
    glVertex3f(-1.0f,  1.0f,  1.0f);
    
    // Back Face
    glColor3f(0.0f, 1.0f, 0.0f);
    glVertex3f(-1.0f, -1.0f, -1.0f);
    glVertex3f(-1.0f,  1.0f, -1.0f);
    glVertex3f( 1.0f,  1.0f, -1.0f);
    glVertex3f( 1.0f, -1.0f, -1.0f);
    
    // Top Face
    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex3f(-1.0f,  1.0f, -1.0f);
    glVertex3f(-1.0f,  1.0f,  1.0f);
    glVertex3f( 1.0f,  1.0f,  1.0f);
    glVertex3f( 1.0f,  1.0f, -1.0f);
    
    // Bottom Face
    glColor3f(1.0f, 1.0f, 0.0f);
    glVertex3f(-1.0f, -1.0f, -1.0f);
    glVertex3f( 1.0f, -1.0f, -1.0f);
    glVertex3f( 1.0f, -1.0f,  1.0f);
    glVertex3f(-1.0f, -1.0f,  1.0f);
    
    // Right face
    glColor3f(1.0f, 0.0f, 1.0f);
    glVertex3f( 1.0f, -1.0f, -1.0f);
    glVertex3f( 1.0f,  1.0f, -1.0f);
    glVertex3f( 1.0f,  1.0f,  1.0f);
    glVertex3f( 1.0f, -1.0f,  1.0f);
    
    // Left Face
    glColor3f(0.0f, 1.0f, 1.0f);
    glVertex3f(-1.0f, -1.0f, -1.0f);
    glVertex3f(-1.0f, -1.0f,  1.0f);
    glVertex3f(-1.0f,  1.0f,  1.0f);
    glVertex3f(-1.0f,  1.0f, -1.0f);
    
    glEnd();
    glPopMatrix();
}

void initScene(void) {
    glEnable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_NORMALIZE);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint32_t width = 1280, height = 800;
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/dri/card0 failed");
        return 1;
    }

    struct drm_version ver;
    char name[32]; ver.name = name; ver.name_len = 32;
    ver.date = NULL; ver.desc = NULL;
    if (ioctl(fd, DRM_IOCTL_VERSION, &ver) < 0) {
        perror("DRM_IOCTL_VERSION failed");
        return 1;
    }
    printf("DRM Driver: %s\n", name);

    struct drm_mode_map_dumb md;
    md.handle = 0xF0B0;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        perror("Mapping hardware FB failed");
        return 1;
    }

    size_t size = 1280 * 800 * 4;
    void *fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (fb == MAP_FAILED) {
        perror("mmap hardware FB failed");
        return 1;
    }

    printf("Starting TinyGL DRM Hello World renderer (%ux%u)...\n", width, height);

    ZBuffer *frameBuffer = ZB_open(width, height, ZB_MODE_RGBA, 0);
    glInit(frameBuffer);

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glViewport(0, 0, width, height);
    
    GLfloat h = (GLfloat)height / (GLfloat)width;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -h, h, 1.0, 60.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    initScene();

    struct timeval tv;
    gettimeofday(&tv, NULL);
    long start_time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    int frames = 0;

    while (1) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw();
        ZB_copyFrameBuffer(frameBuffer, fb, 1280 * 4);
        frames++;

        gettimeofday(&tv, NULL);
        long current_time = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        if (current_time - start_time >= 5000) {
            float fps = frames / 5.0f;
            printf("[DRM] Hello %d frames in 5.0 seconds = %.3f FPS\n", frames, fps);
            frames = 0;
            start_time = current_time;
        }
    }

    return 0;
}
