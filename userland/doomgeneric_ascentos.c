// doomgeneric_ascentos.c — AscentOS platform backend for doomgeneric
// Uses /dev/fb0 (framebuffer), scancode keyboard, /dev/dsp (SB16 OSS audio)

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "doomgeneric.h"
#include "doomkeys.h"

// ── Framebuffer ────────────────────────────────────────────────────────────
static int fb_fd = -1;
static uint32_t *fb_mmap = NULL; // Direct mmap of framebuffer (zero-copy)
static uint32_t fb_width = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch = 0; // Bytes per row (width * 4)
static uint32_t fb_bpp = 32;

// Centering offset for DOOM's 640×400 output
static uint32_t fb_x_offset = 0;
static uint32_t fb_y_offset = 0;

// FPS counter state
static uint32_t fps_frame_count = 0;
static uint32_t fps_last_time = 0;
static uint32_t fps_current = 0;

// 3x5 pixel digit font (bitmask for each row)
static const uint8_t digit_font[10][5] = {
    {0xE, 0xA, 0xA, 0xA, 0xE}, // 0
    {0x4, 0xC, 0x4, 0x4, 0xE}, // 1
    {0xE, 0x2, 0xE, 0x8, 0xE}, // 2
    {0xE, 0x2, 0xE, 0x2, 0xE}, // 3
    {0xA, 0xA, 0xE, 0x2, 0x2}, // 4
    {0xE, 0x8, 0xE, 0x2, 0xE}, // 5
    {0xE, 0x8, 0xE, 0xA, 0xE}, // 6
    {0xE, 0x2, 0x2, 0x2, 0x2}, // 7
    {0xE, 0xA, 0xE, 0xA, 0xE}, // 8
    {0xE, 0xA, 0xE, 0x2, 0xE}, // 9
};

// Draw a single digit at position (x,y) with scale and color
static void draw_digit(uint32_t x, uint32_t y, int digit, int scale,
                       uint32_t color) {
  if (digit < 0 || digit > 9)
    return;
  const uint8_t *glyph = digit_font[digit];
  for (int row = 0; row < 5; row++) {
    uint8_t bits = glyph[row];
    for (int col = 0; col < 3; col++) {
      if (bits & (1 << (2 - col))) {
        for (int sy = 0; sy < scale; sy++) {
          for (int sx = 0; sx < scale; sx++) {
            uint32_t px = x + col * scale + sx;
            uint32_t py = y + row * scale + sy;
            if (px < fb_width && py < fb_height) {
              fb_mmap[py * (fb_pitch / 4) + px] = color;
            }
          }
        }
      }
    }
  }
}

// Draw FPS number at top-right corner
static void draw_fps(void) {
  uint32_t fps = fps_current;
  if (fps > 999)
    fps = 999; // Cap display
  int scale = 2;
  int digit_w = 3 * scale + 1; // 3px wide + 1px spacing
  uint32_t color = 0xFFFFFFFF; // White

  // Position at top-right with margin
  uint32_t start_x = fb_width - 10 - digit_w * 3;
  uint32_t start_y = 10;

  // Draw digits right to left
  int len = 0;
  uint32_t tmp = fps;
  if (tmp == 0) {
    draw_digit(start_x, start_y, 0, scale, color);
    return;
  }
  while (tmp > 0 && len < 3) {
    int d = tmp % 10;
    tmp /= 10;
    draw_digit(start_x + digit_w * (2 - len), start_y, d, scale, color);
    len++;
  }
}

// ── Keyboard (scancode interface) ──────────────────────────────────────────
#define KBDSCANMODE_SET 0x5471
#define KBDSCANCODE_READ 0x5472

typedef struct {
  uint8_t scancode;
  uint8_t is_extended;
  uint8_t is_release;
} scancode_event_t;

// ── Key queue ──────────────────────────────────────────────────────────────
#define KEYQUEUE_SIZE 64
static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueReadIndex = 0;
static unsigned int s_KeyQueueWriteIndex = 0;

static void addKeyToQueue(int pressed, unsigned char doomKey) {
  s_KeyQueue[s_KeyQueueWriteIndex] = (pressed << 8) | doomKey;
  s_KeyQueueWriteIndex++;
  s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}

// ── PS/2 Set 1 scancode → DOOM key mapping ────────────────────────────────
// Based on doomgeneric_soso.c working implementation
static unsigned char scancode_to_doom(uint8_t sc, bool extended) {
  // Extended scancodes (0xE0 prefix)
  if (extended) {
    switch (sc) {
    case 0x48:
      return KEY_UPARROW;
    case 0x50:
      return KEY_DOWNARROW;
    case 0x4B:
      return KEY_LEFTARROW;
    case 0x4D:
      return KEY_RIGHTARROW;
    case 0x1C:
      return KEY_ENTER; // numpad enter
    case 0x35:
      return '/'; // numpad divide
    case 0x38:
      return KEY_RALT; // right alt
    case 0x1D:
      return KEY_FIRE; // right ctrl -> fire (like left ctrl)
    default:
      return 0;
    }
  }

  switch (sc) {
  // Number row
  case 0x02:
    return '1';
  case 0x03:
    return '2';
  case 0x04:
    return '3';
  case 0x05:
    return '4';
  case 0x06:
    return '5';
  case 0x07:
    return '6';
  case 0x08:
    return '7';
  case 0x09:
    return '8';
  case 0x0A:
    return '9';
  case 0x0B:
    return '0';
  case 0x0C:
    return '-';
  case 0x0D:
    return '=';
  // QWERTY row (lowercase for DOOM key bindings)
  case 0x10:
    return 'q';
  case 0x11:
    return 'w';
  case 0x12:
    return 'e';
  case 0x13:
    return 'r';
  case 0x14:
    return 't';
  case 0x15:
    return 'y';
  case 0x16:
    return 'u';
  case 0x17:
    return 'i';
  case 0x18:
    return 'o';
  case 0x19:
    return 'p';
  case 0x1A:
    return '[';
  case 0x1B:
    return ']';
  // Home row (lowercase for DOOM key bindings)
  case 0x1E:
    return 'a';
  case 0x1F:
    return 's';
  case 0x20:
    return 'd';
  case 0x21:
    return 'f';
  case 0x22:
    return 'g';
  case 0x23:
    return 'h';
  case 0x24:
    return 'j';
  case 0x25:
    return 'k';
  case 0x26:
    return 'l';
  case 0x27:
    return ';';
  case 0x28:
    return '\'';
  // Bottom row (lowercase for DOOM key bindings)
  case 0x2C:
    return 'z';
  case 0x2D:
    return 'x';
  case 0x2E:
    return 'c';
  case 0x2F:
    return 'v';
  case 0x30:
    return 'b';
  case 0x31:
    return 'n';
  case 0x32:
    return 'm';
  case 0x33:
    return ',';
  case 0x34:
    return '.';
  // Special keys
  case 0x01:
    return KEY_ESCAPE;
  case 0x0E:
    return KEY_BACKSPACE;
  case 0x0F:
    return KEY_TAB;
  case 0x1C:
    return KEY_ENTER;
  case 0x39:
    return KEY_USE; // space -> use (open doors)
  case 0x29:
    return '`';
  case 0x2B:
    return '\\';
  // Modifiers - DOOM uses these for gameplay!
  case 0x2A:
    return KEY_RSHIFT; // left shift -> run
  case 0x36:
    return KEY_RSHIFT; // right shift -> run
  case 0x1D:
    return KEY_FIRE; // left ctrl -> fire
  case 0x38:
    return KEY_RALT; // left alt -> strafe
  // Function keys
  case 0x3B:
    return KEY_F1;
  case 0x3C:
    return KEY_F2;
  case 0x3D:
    return KEY_F3;
  case 0x3E:
    return KEY_F4;
  case 0x3F:
    return KEY_F5;
  case 0x40:
    return KEY_F6;
  case 0x41:
    return KEY_F7;
  case 0x42:
    return KEY_F8;
  case 0x43:
    return KEY_F9;
  case 0x44:
    return KEY_F10;
  case 0x57:
    return KEY_F11;
  case 0x58:
    return KEY_F12;
  // Lock keys
  case 0x3A:
    return KEY_CAPSLOCK;
  case 0x45:
    return KEY_NUMLOCK;
  case 0x46:
    return KEY_SCRLCK;
  default:
    return 0;
  }
}

// ── Detect framebuffer dimensions from /dev/fb0 node size ──────────────────
static int detect_fb_dimensions(int fd) {
  off_t fb_size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  if (fb_size <= 0) {
    fb_width = 1024;
    fb_height = 768;
    fb_pitch = fb_width * 4;
    return 0;
  }

  static const struct {
    uint32_t w, h;
  } resolutions[] = {{640, 480},   {800, 600},   {1024, 600},  {1024, 768},
                     {1152, 864},  {1280, 720},  {1280, 768},  {1280, 800},
                     {1280, 960},  {1280, 1024}, {1360, 768},  {1366, 768},
                     {1400, 1050}, {1440, 900},  {1600, 900},  {1600, 1200},
                     {1680, 1050}, {1920, 800},  {1920, 1080}, {1920, 1200},
                     {2560, 1080}, {2560, 1440}, {3840, 2160}, {0, 0}};

  for (int i = 0; resolutions[i].w != 0; i++) {
    uint32_t w = resolutions[i].w;
    uint32_t h = resolutions[i].h;
    uint32_t pitch = w * 4;
    if ((off_t)(pitch * h) == fb_size) {
      fb_width = w;
      fb_height = h;
      fb_pitch = pitch;
      return 1;
    }
  }

  // Fallback: guess from size
  for (uint32_t w = 640; w <= 4096; w += 64) {
    if (fb_size % (w * 4) == 0) {
      uint32_t h = (uint32_t)(fb_size / (w * 4));
      if (h >= 480 && h <= 2160) {
        fb_width = w;
        fb_height = h;
        fb_pitch = w * 4;
        return 1;
      }
    }
  }

  fb_width = 1024;
  fb_height = 768;
  fb_pitch = fb_width * 4;
  return 0;
}

// ── DG_Init ────────────────────────────────────────────────────────────────
void DG_Init(void) {
  // Open framebuffer
  fb_fd = open("/dev/fb0", O_RDWR);
  if (fb_fd < 0) {
    printf("DOOM: Cannot open /dev/fb0\n");
    exit(1);
  }

  detect_fb_dimensions(fb_fd);

  // Calculate centering offsets
  if (fb_width >= DOOMGENERIC_RESX)
    fb_x_offset = (fb_width - DOOMGENERIC_RESX) / 2;
  if (fb_height >= DOOMGENERIC_RESY)
    fb_y_offset = (fb_height - DOOMGENERIC_RESY) / 2;

  // ── mmap framebuffer for zero-copy direct access ──────────────────────────
  // This eliminates all write() syscalls per frame, giving huge speedup.
  size_t fb_size = (size_t)fb_pitch * fb_height;
  fb_mmap = (uint32_t *)mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                             fb_fd, 0);
  if (fb_mmap == MAP_FAILED) {
    printf("DOOM: Cannot mmap framebuffer (%zu bytes)\n", fb_size);
    exit(1);
  }

  // Clear framebuffer to black
  memset(fb_mmap, 0, fb_size);

  printf("DOOM: Framebuffer %ux%u (pitch=%u), DOOM centered at (%u,%u)\n",
         fb_width, fb_height, fb_pitch, fb_x_offset, fb_y_offset);

  // Enable scancode mode on stdin for raw keyboard input
  if (ioctl(0, KBDSCANMODE_SET, 1) < 0) {
    printf("DOOM: Warning: Failed to enable scancode mode\n");
  }

  // Make stdin non-blocking
  int flags = fcntl(0, F_GETFL, 0);
  fcntl(0, F_SETFL, flags | O_NONBLOCK);
}

// ── Handle keyboard input via scancode interface ───────────────────────────
static void handleKeyInput(void) {
  scancode_event_t event;

  while (ioctl(0, KBDSCANCODE_READ, &event) > 0) {
    unsigned char doomKey = scancode_to_doom(event.scancode, event.is_extended);
    if (doomKey == 0)
      continue;

    int pressed = !event.is_release;
    addKeyToQueue(pressed, doomKey);
  }
}

// ── DG_DrawFrame ───────────────────────────────────────────────────────────
void DG_DrawFrame(void) {
  if (!fb_mmap)
    return;

  // ── Zero-copy blit: write directly to mmap'd framebuffer ──────────────────
  // No syscalls per frame! Just a simple memcpy per row.
  // Source: DG_ScreenBuffer (640x400 = DOOMGENERIC_RESX x DOOMGENERIC_RESY)
  // Dest:   fb_mmap at centered offset

  uint32_t *src = (uint32_t *)DG_ScreenBuffer;

  for (int y = 0; y < DOOMGENERIC_RESY; y++) {
    uint32_t *dest_row =
        fb_mmap + (y + fb_y_offset) * (fb_pitch / 4) + fb_x_offset;
    uint32_t *src_row = src + y * DOOMGENERIC_RESX;

    // Copy one row (320 pixels = DOOMGENERIC_RESX)
    for (int x = 0; x < DOOMGENERIC_RESX; x++) {
      dest_row[x] = src_row[x];
    }
  }

  // ── FPS counter ────────────────────────────────────────────────────────
  fps_frame_count++;
  uint32_t now = DG_GetTicksMs();
  if (now - fps_last_time >= 1000) {
    fps_current = fps_frame_count;
    fps_frame_count = 0;
    fps_last_time = now;
  }
  draw_fps();

  // Process keyboard input
  handleKeyInput();
}

// ── DG_SleepMs ─────────────────────────────────────────────────────────────
void DG_SleepMs(uint32_t ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  syscall(SYS_nanosleep, &ts, NULL);
}

// ── DG_GetTicksMs ──────────────────────────────────────────────────────────
uint32_t DG_GetTicksMs(void) {
  struct timespec ts;
  syscall(SYS_clock_gettime, 1, &ts); // CLOCK_MONOTONIC = 1
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ── DG_GetKey ──────────────────────────────────────────────────────────────
int DG_GetKey(int *pressed, unsigned char *key) {
  if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex) {
    return 0; // queue empty
  }

  unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
  s_KeyQueueReadIndex++;
  s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

  *pressed = keyData >> 8;
  *key = keyData & 0xFF;
  return 1;
}

// ── DG_SetWindowTitle ──────────────────────────────────────────────────────
void DG_SetWindowTitle(const char *title) {
  (void)title; // no window manager, no title
}

// ── main ───────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
  doomgeneric_Create(argc, argv);

  while (1) {
    doomgeneric_Tick();
  }

  return 0;
}
