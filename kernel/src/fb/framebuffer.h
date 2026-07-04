#ifndef FB_FRAMEBUFFER_H
#define FB_FRAMEBUFFER_H

#include "../fs/vfs.h"
#include <limine.h>
#include <stdbool.h>
#include <stdint.h>

// Display backend types
typedef enum {
  FB_BACKEND_LIMINE,  // Limine framebuffer (fallback)
  FB_BACKEND_DRM,     // DRM device
} fb_backend_t;

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602

#define KD_TEXT 0x00
#define KD_GRAPHICS 0x01

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

void fb_init(struct limine_framebuffer *framebuffer);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color);
void fb_clear(uint32_t color);
void fb_draw_glyph_scanline(uint32_t x, uint32_t y, uint8_t bits, uint32_t fg,
                            uint32_t bg);
uint32_t fb_get_width(void);
uint32_t fb_get_height(void);
void *fb_get_base(void);
uint32_t fb_get_pitch(void);
uint16_t fb_get_bpp(void);
int fb_get_kd_mode(void);
void fb_set_kd_mode(int mode);

void fb_set_backbuffer_mode(bool enabled);
bool fb_is_backbuffer_enabled(void);
void *fb_get_backbuffer(void);
void fb_swap_buffer(void);
void fb_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

void fb_register_vfs(void);
void fb_register_device_node(const char *name, vfs_node_t *node);
vfs_node_t *fb_lookup_device(const char *name);

// Backend selection and querying
fb_backend_t fb_get_backend(void);
const char *fb_get_backend_name(void);
void fb_detect_drm_backend(void);  // Call this after DRM is registered

#endif
