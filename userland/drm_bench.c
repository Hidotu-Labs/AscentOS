#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define DRM_IOCTL_MODE_MAP_DUMB     0xC01064B3
struct drm_mode_map_dumb {
    uint32_t handle, pad;
    uint64_t offset;
};

double get_time() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

int main() {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/dri/card0");
        return 1;
    }

    // Map the hardware framebuffer (handle 0xF0B0)
    struct drm_mode_map_dumb md;
    md.handle = 0xF0B0;
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
        perror("DRM MAP failed");
        return 1;
    }

    size_t size = 1280 * 800 * 4; // ~4MB
    uint32_t *drm_fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (drm_fb == MAP_FAILED) {
        perror("mmap DRM failed");
        return 1;
    }

    // Allocate a standard buffer for comparison
    uint32_t *ram_buf = malloc(size);
    memset(ram_buf, 0xAA, size);

    printf("--- AscentOS DRM Bandwidth Benchmark ---\n");
    printf("Target: 1280x800 Display (~4.09 MB per frame)\n\n");

    // Test 1: RAM to RAM (Standard speed)
    uint32_t *dummy_buf = malloc(size);
    double start = get_time();
    for(int i=0; i<500; i++) {
        memcpy(dummy_buf, ram_buf, size);
    }
    double end = get_time();
    printf("RAM-to-RAM Copy:    %.2f GB/s\n", (size * 500.0 / (end - start)) / (1024*1024*1024));

    // Test 2: RAM to DRM Hardware (The Real Test)
    start = get_time();
    for(int i=0; i<500; i++) {
        // We write different colors to see it on screen
        uint32_t color = (i % 2) ? 0xFF0000 : 0x00FF00;
        for(size_t j=0; j<size/4; j++) drm_fb[j] = color;
    }
    end = get_time();
    printf("RAM-to-DRM (WC):    %.2f GB/s\n", (size * 500.0 / (end - start)) / (1024*1024*1024));

    printf("\nTest Complete. If the screen flashed Red/Green rapidly, \n");
    printf("the DRM Write-Combining is fully active!\n");

    return 0;
}
