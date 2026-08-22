#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <errno.h>

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
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
};

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

static double get_time_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

// Alignment-safe 64-bit streaming store fill
static inline void fill_pixels(uint32_t *dst, size_t count, uint32_t color) {
    if (count == 0) return;
    uint64_t col64 = ((uint64_t)color << 32) | color;
    uint64_t *d64 = (uint64_t *)dst;

    if ((uintptr_t)d64 & 4) {
        *(uint32_t *)d64 = color;
        d64 = (uint64_t *)((uint32_t *)d64 + 1);
        count--;
    }

    size_t qwords = count >> 1;
    __asm__ volatile(
        "rep stosq"
        : "+D"(d64), "+c"(qwords)
        : "a"(col64)
        : "memory"
    );

    if (count & 1) {
        *(uint32_t *)d64 = color;
    }
}

// Branchless 8-pixel font glyph unpacking
static inline void draw_8pixels_branchless(uint32_t *line, uint8_t bits,
                                           uint32_t fg, uint32_t bg) {
    uint64_t p0 = (bits & 0x80) ? fg : bg;
    uint64_t p1 = (bits & 0x40) ? fg : bg;
    uint64_t p2 = (bits & 0x20) ? fg : bg;
    uint64_t p3 = (bits & 0x10) ? fg : bg;
    uint64_t p4 = (bits & 0x08) ? fg : bg;
    uint64_t p5 = (bits & 0x04) ? fg : bg;
    uint64_t p6 = (bits & 0x02) ? fg : bg;
    uint64_t p7 = (bits & 0x01) ? fg : bg;

    uint64_t *d64 = (uint64_t *)line;
    d64[0] = (p1 << 32) | p0;
    d64[1] = (p3 << 32) | p2;
    d64[2] = (p5 << 32) | p4;
    d64[3] = (p7 << 32) | p6;
}

int main(void) {
    printf("====================================================\n");
    printf("AvoryOS High-Performance Framebuffer Benchmark Suite\n");
    printf("====================================================\n\n");

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/fb0 failed");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("ioctl FBIOGET_VSCREENINFO failed");
        close(fd);
        return 1;
    }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("ioctl FBIOGET_FSCREENINFO failed");
        close(fd);
        return 1;
    }

    uint32_t width = vinfo.xres;
    uint32_t height = vinfo.yres;
    uint32_t pitch = finfo.line_length;
    size_t fb_size = (size_t)height * pitch;

    printf("[FB] Display: %ux%u @ %u bpp, pitch=%u bytes, size=%.2f MB\n\n",
           width, height, vinfo.bits_per_pixel, pitch, (double)fb_size / (1024.0 * 1024.0));

    void *fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("mmap /dev/fb0 failed");
        close(fd);
        return 1;
    }

    // -------------------------------------------------------------------------
    // Test 1: Fullscreen Direct VRAM Fill Bandwidth
    // -------------------------------------------------------------------------
    int num_frames = 200;
    printf("[TEST 1] Fullscreen Direct VRAM Fill Benchmark (%d frames)...\n", num_frames);
    uint32_t colors[] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFF00, 0x00000000};
    size_t num_colors = sizeof(colors) / sizeof(colors[0]);

    double t0 = get_time_sec();
    for (int f = 0; f < num_frames; f++) {
        uint32_t color = colors[f % num_colors];
        fill_pixels((uint32_t *)fb_mem, fb_size / 4, color);
    }
    double t1 = get_time_sec();
    double dt = t1 - t0;
    double fps = (double)num_frames / dt;
    double gb_sec = ((double)num_frames * (double)fb_size) / (dt * 1024.0 * 1024.0 * 1024.0);

    printf("[PASS]   %d full-screen frames rendered!\n", num_frames);
    printf("[BENCH]  Total Time:       %.4f seconds\n", dt);
    printf("[BENCH]  Throughput:       %.1f FPS\n", fps);
    printf("[BENCH]  VRAM Bandwidth:   %.2f GB/sec\n\n", gb_sec);

    // -------------------------------------------------------------------------
    // Test 2: Tiled Rectangles / Window Drawing (10,000 Rectangles)
    // -------------------------------------------------------------------------
    int num_rects = 10000;
    uint32_t rw = 200, rh = 200;
    printf("[TEST 2] Tiled Window Rect Fill Benchmark (%d rects %ux%u)...\n",
           num_rects, rw, rh);

    t0 = get_time_sec();
    for (int i = 0; i < num_rects; i++) {
        uint32_t rx = (i * 37) % (width - rw);
        uint32_t ry = (i * 53) % (height - rh);
        uint32_t color = 0x00336699 ^ (i << 8);

        for (uint32_t r = 0; r < rh; r++) {
            uint32_t *line = (uint32_t *)((uint8_t *)fb_mem + (ry + r) * pitch + rx * 4);
            fill_pixels(line, rw, color);
        }
    }
    t1 = get_time_sec();
    dt = t1 - t0;
    double rects_per_sec = (double)num_rects / dt;

    printf("[PASS]   %d rectangles filled!\n", num_rects);
    printf("[BENCH]  Total Time:       %.4f seconds\n", dt);
    printf("[BENCH]  Fill Rate:        %.0f rects/sec\n\n", rects_per_sec);

    // -------------------------------------------------------------------------
    // Test 3: Branchless Font Glyph Blitting (1,000,000 scanlines)
    // -------------------------------------------------------------------------
    int num_glyphs = 1000000;
    printf("[TEST 3] Branchless Font Glyph Rasterization (%d scanlines)...\n", num_glyphs);
    _Alignas(16) uint32_t glyph_buf[8];

    t0 = get_time_sec();
    for (int i = 0; i < num_glyphs; i++) {
        uint8_t bits = (uint8_t)(i ^ (i >> 3));
        draw_8pixels_branchless(glyph_buf, bits, 0x00FFFFFF, 0x00000000);
        // Prevent compiler from completely optimizing out the loop
        __asm__ volatile("" : : "r"(glyph_buf) : "memory");
    }
    t1 = get_time_sec();
    dt = t1 - t0;
    if (dt < 0.0001) dt = 0.0001;
    double glyphs_per_sec = (double)num_glyphs / dt;

    printf("[PASS]   %d glyph scanlines rasterized!\n", num_glyphs);
    printf("[BENCH]  Total Time:       %.4f seconds\n", dt);
    printf("[BENCH]  Glyph Rate:       %.0f scanlines/sec (%.2f Million/sec)\n\n",
           glyphs_per_sec, glyphs_per_sec / 1000000.0);

    // Clear back to black
    fill_pixels((uint32_t *)fb_mem, fb_size / 4, 0x00000000);

    munmap(fb_mem, fb_size);
    close(fd);

    printf("====================================================\n");
    printf("ALL FRAMEBUFFER BENCHMARKS COMPLETED SUCCESSFULLY!\n");
    printf("====================================================\n");
    return 0;
}
