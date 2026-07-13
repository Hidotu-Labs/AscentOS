#include "klog.h"
#include "../drivers/serial.h"
#include "../fb/framebuffer.h"
#include "../font/font.h"
#include "../lock/spinlock.h"
#include "console.h"

static spinlock_t klog_lock = SPINLOCK_INIT;
static bool screen_logging_enabled = false;
static uint32_t screen_x = 0;
static uint32_t screen_y = 0;

#define KLOG_FG 0x00FFFFFF
#define KLOG_BG 0x00000000

static uint32_t klog_fg = KLOG_FG;

static bool esc = false;
static int esc_state = 0;
static char esc_buffer[32];
static int esc_idx = 0;

static uint32_t klog_ansi_colors[8] = {
    0x00000000, // Black
    0x00FF0000, // Red
    0x0000FF00, // Green
    0x00FFFF00, // Yellow
    0x000000FF, // Blue
    0x00FF00FF, // Magenta
    0x0000FFFF, // Cyan
    0x00FFFFFF  // White
};

void klog_set_screen_logging(bool enabled) {
  spinlock_acquire(&klog_lock);
  screen_logging_enabled = enabled;
  spinlock_release(&klog_lock);
}

static void klog_putchar_screen(char c) {
  if (!screen_logging_enabled)
    return;

  if (esc) {
    if (esc_state == 0) {
      if (c == '[') {
        esc_state = 1;
      } else {
        esc = false;
      }
    } else if (esc_state == 1) {
      if (c >= '0' && c <= '9' || c == ';') {
        if (esc_idx < 31)
          esc_buffer[esc_idx++] = c;
      } else if (c == 'm') {
        esc_buffer[esc_idx] = '\0';
        int code = 0;
        for (int i = 0; esc_buffer[i]; i++) {
          if (esc_buffer[i] == ';') {
            // Very basic: just take the last one or handle multiple?
            // Let's just handle single codes for now.
            code = 0;
            continue;
          }
          code = code * 10 + (esc_buffer[i] - '0');
        }

        if (code == 0) {
          klog_fg = KLOG_FG;
        } else if (code >= 30 && code <= 37) {
          klog_fg = klog_ansi_colors[code - 30];
        } else if (code >= 90 && code <= 97) {
            // Bright colors
            klog_fg = klog_ansi_colors[code - 90]; // Just use same for now or brighten?
        }

        esc = false;
      } else {
        esc = false;
      }
    }
    return;
  }

  if (c == '\x1b') {
    esc = true;
    esc_state = 0;
    esc_idx = 0;
    return;
  }

  uint32_t w = fb_get_width();
  uint32_t h = fb_get_height();

  if (c == '\n') {
    screen_x = 0;
    screen_y += FONT_HEIGHT;
  } else if (c == '\r') {
    screen_x = 0;
  } else {
    if (screen_x + FONT_WIDTH > w) {
      screen_x = 0;
      screen_y += FONT_HEIGHT;
    }

    if (screen_y + FONT_HEIGHT > h) {
      // For early boot logging, we don't handle scrolling properly yet,
      // just wrap around or stop? Let's just wrap around for now.
      screen_y = 0;
    }

    const uint8_t *glyph = font_get_glyph(c);
    for (uint32_t gy = 0; gy < FONT_HEIGHT; gy++) {
      fb_draw_glyph_scanline(screen_x, screen_y + gy, glyph[gy], klog_fg,
                             KLOG_BG);
    }
    screen_x += FONT_WIDTH;
  }
}

void klog_putchar(char c) {
  spinlock_acquire(&klog_lock);
  serial_putchar(c);
  klog_putchar_screen(c);
  spinlock_release(&klog_lock);
}

void klog_puts(const char *s) {
  spinlock_acquire(&klog_lock);
  const char *p = s;
  while (*p)
    p++;
  serial_write(s, (size_t)(p - s));
  while (*s)
    klog_putchar_screen(*s++);
  spinlock_release(&klog_lock);
}

void klog_uint64(uint64_t num) {
  spinlock_acquire(&klog_lock);
  if (num == 0) {
    serial_putchar('0');
    klog_putchar_screen('0');
    spinlock_release(&klog_lock);
    return;
  }
  char buf[20];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    i--;
    serial_putchar(buf[i]);
    klog_putchar_screen(buf[i]);
  }
  spinlock_release(&klog_lock);
}

void klog_hex64(uint64_t num) {
  spinlock_acquire(&klog_lock);
  const char *hex = "0123456789ABCDEF";
  serial_putchar('0');
  klog_putchar_screen('0');
  serial_putchar('x');
  klog_putchar_screen('x');
  for (int i = 60; i >= 0; i -= 4) {
    char c = hex[(num >> i) & 0xF];
    serial_putchar(c);
    klog_putchar_screen(c);
  }
  spinlock_release(&klog_lock);
}

void klog_hex32(uint32_t num) {
  spinlock_acquire(&klog_lock);
  const char *hex = "0123456789ABCDEF";
  serial_putchar('0');
  klog_putchar_screen('0');
  serial_putchar('x');
  klog_putchar_screen('x');
  for (int i = 28; i >= 0; i -= 4) {
    char c = hex[(num >> i) & 0xF];
    serial_putchar(c);
    klog_putchar_screen(c);
  }
  spinlock_release(&klog_lock);
}
