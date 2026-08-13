#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

// ---------------------------------------------------------------------------
// Linux framebuffer ioctls
// ---------------------------------------------------------------------------
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield { uint32_t offset, length, msb_right; };
struct fb_var_screeninfo {
  uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
  uint32_t bits_per_pixel, grayscale;
  struct fb_bitfield red, green, blue, transp;
  uint32_t nonstd, activate, height, width, accel_flags, pixclock;
  uint32_t left_margin, right_margin, upper_margin, lower_margin;
  uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
  uint32_t reserved[4];
};
struct fb_fix_screeninfo {
  char id[16]; unsigned long smem_start; uint32_t smem_len, type, type_aux;
  uint32_t visual; uint16_t xpanstep, ypanstep, ywrapstep; uint32_t line_length;
  unsigned long mmio_start; uint32_t mmio_len, accel; uint16_t capabilities, reserved[2];
};

// ---------------------------------------------------------------------------
// OSS audio ioctls (same as booter.c / audio_dsp.c)
// ---------------------------------------------------------------------------
#define SNDCTL_DSP_SPEED    0xC0045002
#define SNDCTL_DSP_STEREO   0xC0045003
#define SNDCTL_DSP_SETFMT   0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define AFMT_S16_LE         0x00000010

// Sound event IDs
#define SND_NONE        0
#define SND_PADDLE_HIT  1
#define SND_WALL_HIT    2
#define SND_SCORE       3

// ---------------------------------------------------------------------------
// Game constants
// ---------------------------------------------------------------------------
#define PADDLE_WIDTH  15
#define PADDLE_HEIGHT 80
#define BALL_SIZE     12
#define PADDLE_SPEED  11
#define BALL_SPEED    7
#define AI_SPEED      5
#define BALL_MAX_X    17
#define BALL_MAX_Y    11

// Colors 0xAARRGGBB
#define COLOR_BG      0xFF1a1a2e
#define COLOR_PADDLE  0xFF00d4ff
#define COLOR_BALL    0xFFff6b6b
#define COLOR_NET     0xFF4a4a6a
#define COLOR_TEXT    0xFFffffff
#define COLOR_SCORE   0xFFe0e0ff

// Score limit
#define SCORE_LIMIT   9

// ---------------------------------------------------------------------------
// 8x16 font bitmaps — digits 0-9 only, extracted from kernel's font.c
// ---------------------------------------------------------------------------
#define FONT_W 8
#define FONT_H 16
static const uint8_t digit_glyphs[10][FONT_H] = {
  /* 0 */ {0x00,0x00,0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
  /* 1 */ {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00},
  /* 2 */ {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00},
  /* 3 */ {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
  /* 4 */ {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00,0x00},
  /* 5 */ {0x00,0x00,0xFE,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
  /* 6 */ {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
  /* 7 */ {0x00,0x00,0xFE,0xC6,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00},
  /* 8 */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00},
  /* 9 */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00,0x00},
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static int      fb_fd  = -1;
static int      tty_fd = -1;
static uint8_t *fb_mem = NULL;
static uint32_t fb_width  = 0;
static uint32_t fb_height = 0;
static uint32_t fb_pitch  = 0;
static int use_x11 = 0;
static Display *x_display = NULL;
static Window x_window;
static GC x_gc;
static Atom wm_delete_window;
static int move_up = 0;
static int move_down = 0;
static int running = 1;

static struct termios orig_termios;

// Sound: write end of pipe to child, read end in child
static int  snd_pipe[2] = { -1, -1 };
static pid_t snd_pid = -1;

static int player_y     = 0;
static int ai_y         = 0;
static int ball_x       = 0;
static int ball_y       = 0;
static int ball_vx      = BALL_SPEED;
static int ball_vy      = 0;
static int player_score = 0;
static int ai_score     = 0;

// ---------------------------------------------------------------------------
// Cleanup / signals
// ---------------------------------------------------------------------------
static void cleanup(void) {
  if (!use_x11) tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);

  /* Kill sound child */
  if (snd_pipe[1] >= 0) { close(snd_pipe[1]); snd_pipe[1] = -1; }
  if (snd_pipe[0] >= 0) { close(snd_pipe[0]); snd_pipe[0] = -1; }
  if (snd_pid > 0) { kill(snd_pid, SIGTERM); waitpid(snd_pid, NULL, 0); snd_pid = -1; }

  if (x_display) {
    if (x_gc) XFreeGC(x_display, x_gc);
    if (x_window) XDestroyWindow(x_display, x_window);
    XCloseDisplay(x_display); x_display = NULL;
  }
  if (fb_mem)     { free(fb_mem);  fb_mem  = NULL; }
  if (fb_fd >= 0) { close(fb_fd);  fb_fd   = -1;   }
  if (tty_fd >= 0){ close(tty_fd); tty_fd  = -1;   }
}

static void sig_handler(int sig) { (void)sig; cleanup(); _exit(0); }

// ---------------------------------------------------------------------------
// Sound child process
// Reads a 1-byte sound-event ID from the pipe and plays the matching tone
// to /dev/dsp using synthesised 16-bit PCM (same OSS pattern as booter.c).
// ---------------------------------------------------------------------------
#define DSP_RATE   22050
#define DSP_CHANS  1

/* Generate and write a pure sine tone of the given frequency and duration */
static void play_tone(int dsp, int freq_hz, int dur_ms) {
  int samples = (DSP_RATE * dur_ms) / 1000;
  if (samples <= 0) return;
  int16_t *buf = malloc((size_t)samples * sizeof(int16_t));
  if (!buf) return;

  for (int i = 0; i < samples; i++) {
    /* Amplitude 12000, with a simple linear fade-out to reduce clicking */
    double t   = (double)i / DSP_RATE;
    double env = 1.0 - (double)i / samples; /* fade out */
    buf[i] = (int16_t)(12000.0 * env * sin(2.0 * 3.14159265 * freq_hz * t));
  }

  size_t total = (size_t)samples * sizeof(int16_t);
  size_t done  = 0;
  while (done < total) {
    ssize_t n = write(dsp, (uint8_t *)buf + done, total - done);
    if (n <= 0) break;
    done += (size_t)n;
  }
  free(buf);
}

static void sound_child(int read_fd) {
  /* Close the write end — we only read */
  close(snd_pipe[1]);

  int dsp = open("/dev/dsp", O_WRONLY);
  if (dsp < 0) {
    /* No audio device — silently discard events and exit when pipe closes */
    uint8_t ev;
    while (read(read_fd, &ev, 1) == 1) {}
    _exit(0);
  }

  int rate = DSP_RATE;
  ioctl(dsp, SNDCTL_DSP_SPEED,    &rate);
  int ch   = DSP_CHANS;
  ioctl(dsp, SNDCTL_DSP_CHANNELS, &ch);
  int fmt  = AFMT_S16_LE;
  ioctl(dsp, SNDCTL_DSP_SETFMT,   &fmt);

  uint8_t ev;
  while (read(read_fd, &ev, 1) == 1) {
    switch (ev) {
      case SND_PADDLE_HIT: play_tone(dsp, 480, 40);  break; /* high beep */
      case SND_WALL_HIT:   play_tone(dsp, 240, 30);  break; /* mid beep  */
      case SND_SCORE:      play_tone(dsp, 110, 300); break; /* low buzz  */
    }
  }
  close(dsp);
  _exit(0);
}

static void sound_emit(uint8_t ev) {
  if (snd_pipe[1] >= 0)
    write(snd_pipe[1], &ev, 1);
}

// ---------------------------------------------------------------------------
// Terminal setup
// ---------------------------------------------------------------------------
static int setup_terminal(void) {
  if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) return -1;

  struct termios raw = orig_termios;
  raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
  raw.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  raw.c_cflag |= CS8;
  raw.c_oflag &= ~(unsigned)OPOST;
  raw.c_cc[VMIN]  = 0;
  raw.c_cc[VTIME] = 0;

  return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// ---------------------------------------------------------------------------
// Framebuffer setup
// ---------------------------------------------------------------------------
static int setup_framebuffer(void) {
  fb_fd = open("/dev/fb0", O_RDWR);
  if (fb_fd < 0) { perror("open /dev/fb0"); return -1; }

  struct fb_var_screeninfo vinfo;
  memset(&vinfo, 0, sizeof(vinfo));
  if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
    fb_width  = vinfo.xres;
    fb_height = vinfo.yres;
    fb_pitch  = vinfo.xres_virtual * (vinfo.bits_per_pixel / 8);
    if (fb_pitch < fb_width * 4) fb_pitch = fb_width * 4;
  } else {
    struct fb_fix_screeninfo finfo;
    memset(&finfo, 0, sizeof(finfo));
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0 && finfo.line_length) {
      fb_pitch  = finfo.line_length;
      fb_height = finfo.smem_len / fb_pitch;
      fb_width  = fb_pitch / 4;
    } else {
      fb_width = 1024; fb_height = 768; fb_pitch = fb_width * 4;
    }
  }

  size_t fb_size = (size_t)fb_pitch * fb_height;
  fb_mem = malloc(fb_size);
  if (!fb_mem) { fprintf(stderr, "pong: malloc failed\n"); return -1; }
  memset(fb_mem, 0, fb_size);
  return 0;
}

static int setup_x11(void) {
  x_display = XOpenDisplay(NULL);
  if (!x_display) return -1;

  int screen = DefaultScreen(x_display);
  fb_width = 800;
  fb_height = 600;
  fb_pitch = fb_width * 4;
  x_window = XCreateSimpleWindow(
      x_display, RootWindow(x_display, screen), 0, 0, fb_width, fb_height, 0,
      BlackPixel(x_display, screen), BlackPixel(x_display, screen));
  XStoreName(x_display, x_window, "Pong");
  XSelectInput(x_display, x_window,
               ExposureMask | KeyPressMask | KeyReleaseMask | StructureNotifyMask);

  XSizeHints hints;
  memset(&hints, 0, sizeof(hints));
  hints.flags = PMinSize | PMaxSize;
  hints.min_width = hints.max_width = (int)fb_width;
  hints.min_height = hints.max_height = (int)fb_height;
  XSetWMNormalHints(x_display, x_window, &hints);
  wm_delete_window = XInternAtom(x_display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(x_display, x_window, &wm_delete_window, 1);

  x_gc = XCreateGC(x_display, x_window, 0, NULL);
  XMapWindow(x_display, x_window);
  XFlush(x_display);
  return 0;
}

static int setup_tty(void) {
  tty_fd = open("/dev/tty0", O_RDWR);
  if (tty_fd < 0) tty_fd = open("/dev/console", O_RDWR);
  /* non-fatal */
  return 0;
}

// ---------------------------------------------------------------------------
// Sound subprocess
// ---------------------------------------------------------------------------
static void setup_sound(void) {
  if (pipe(snd_pipe) < 0) { snd_pipe[0] = snd_pipe[1] = -1; return; }

  snd_pid = fork();
  if (snd_pid < 0) {
    close(snd_pipe[0]); close(snd_pipe[1]);
    snd_pipe[0] = snd_pipe[1] = -1;
    return;
  }
  if (snd_pid == 0) {
    sound_child(snd_pipe[0]);
    _exit(0);
  }
  /* Parent: close read end, keep write end */
  close(snd_pipe[0]);
  snd_pipe[0] = -1;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static void fill_rect(int x, int y, int w, int h, uint32_t color) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > (int)fb_width) w = (int)fb_width - x;
  if (y + h > (int)fb_height) h = (int)fb_height - y;
  if (w <= 0 || h <= 0) return;
  for (int dy = 0; dy < h; dy++) {
    uint32_t *p = (uint32_t *)(fb_mem + (uint32_t)(y + dy) * fb_pitch) + x;
    for (int dx = 0; dx < w; dx++) p[dx] = color;
  }
}

static void clear_screen(void) {
  uint32_t *first = (uint32_t *)fb_mem;
  for (uint32_t col = 0; col < fb_width; col++) first[col] = COLOR_BG;
  for (uint32_t row = 1; row < fb_height; row++)
    memcpy(fb_mem + row * fb_pitch, first, (size_t)fb_width * 4);
}

/* Draw one digit glyph at pixel position (px, py), scaled by `scale` */
static void draw_digit(int digit, int px, int py, int scale, uint32_t color) {
  if (digit < 0 || digit > 9) return;
  const uint8_t *g = digit_glyphs[digit];
  for (int row = 0; row < FONT_H; row++) {
    uint8_t bits = g[row];
    for (int col = 0; col < FONT_W; col++) {
      if (bits & (0x80 >> col)) {
        fill_rect(px + col * scale, py + row * scale, scale, scale, color);
      }
    }
  }
}

/* Draw a 1-or-2 digit number centred at cx, top at py */
static void draw_number(int n, int cx, int py, int scale, uint32_t color) {
  n = n > 99 ? 99 : n;
  int gw = FONT_W * scale;

  if (n >= 10) {
    int tens = n / 10, ones = n % 10;
    int total_w = gw * 2 + scale; /* 1 pixel gap between digits */
    int x = cx - total_w / 2;
    draw_digit(tens, x,       py, scale, color);
    draw_digit(ones, x + gw + scale, py, scale, color);
  } else {
    draw_digit(n, cx - gw / 2, py, scale, color);
  }
}

static void draw_net(void) {
  int net_x = (int)fb_width / 2;
  for (int y = 0; y < (int)fb_height; y += 20)
    fill_rect(net_x - 2, y, 4, 10, COLOR_NET);
}

static unsigned long x11_color(uint32_t argb) {
  /* AvoryOS Xorg uses the standard 24-bit TrueColor visual. */
  return (unsigned long)(argb & 0x00ffffffU);
}

static void draw_x11_digit(int digit, int px, int py, int scale) {
  if (digit < 0 || digit > 9) return;
  XRectangle rects[FONT_W * FONT_H];
  int count = 0;
  for (int row = 0; row < FONT_H; row++) {
    uint8_t bits = digit_glyphs[digit][row];
    for (int col = 0; col < FONT_W; col++) {
      if (bits & (0x80 >> col)) {
        rects[count].x = (short)(px + col * scale);
        rects[count].y = (short)(py + row * scale);
        rects[count].width = (unsigned short)scale;
        rects[count].height = (unsigned short)scale;
        count++;
      }
    }
  }
  XFillRectangles(x_display, x_window, x_gc, rects, count);
}

static void draw_x11_number(int number, int center_x, int py, int scale) {
  if (number > 99) number = 99;
  int glyph_width = FONT_W * scale;
  if (number >= 10) {
    int total_width = glyph_width * 2 + scale;
    int x = center_x - total_width / 2;
    draw_x11_digit(number / 10, x, py, scale);
    draw_x11_digit(number % 10, x + glyph_width + scale, py, scale);
  } else {
    draw_x11_digit(number, center_x - glyph_width / 2, py, scale);
  }
}

static void render_x11(void) {
  XSetForeground(x_display, x_gc, x11_color(COLOR_BG));
  XFillRectangle(x_display, x_window, x_gc, 0, 0, fb_width, fb_height);

  XSetForeground(x_display, x_gc, x11_color(COLOR_NET));
  int net_x = (int)fb_width / 2 - 2;
  for (int y = 0; y < (int)fb_height; y += 20)
    XFillRectangle(x_display, x_window, x_gc, net_x, y, 4, 10);

  XSetForeground(x_display, x_gc, x11_color(COLOR_PADDLE));
  XFillRectangle(x_display, x_window, x_gc, 30, player_y,
                 PADDLE_WIDTH, PADDLE_HEIGHT);
  XFillRectangle(x_display, x_window, x_gc,
                 (int)fb_width - 30 - PADDLE_WIDTH, ai_y,
                 PADDLE_WIDTH, PADDLE_HEIGHT);

  XSetForeground(x_display, x_gc, x11_color(COLOR_BALL));
  XFillRectangle(x_display, x_window, x_gc, ball_x, ball_y,
                 BALL_SIZE, BALL_SIZE);

  XSetForeground(x_display, x_gc, x11_color(COLOR_SCORE));
  draw_x11_number(player_score, (int)fb_width / 2 - 60, 20, 3);
  draw_x11_number(ai_score, (int)fb_width / 2 + 60, 20, 3);
  XFlush(x_display);
}

static void render(void) {
  if (use_x11) {
    render_x11();
    return;
  }

  clear_screen();
  draw_net();

  /* Scores — large digits, side by side near top centre */
  int score_y     = 20;
  int score_scale = 3;                         /* 24×48 per digit */
  int gap         = 60;                        /* gap from centre line */
  int cx_player   = (int)fb_width / 2 - gap;
  int cx_ai       = (int)fb_width / 2 + gap;

  draw_number(player_score, cx_player, score_y, score_scale, COLOR_SCORE);
  draw_number(ai_score,     cx_ai,     score_y, score_scale, COLOR_SCORE);

  /* Paddles & ball */
  fill_rect(30, player_y, PADDLE_WIDTH, PADDLE_HEIGHT, COLOR_PADDLE);
  fill_rect((int)fb_width - 30 - PADDLE_WIDTH, ai_y, PADDLE_WIDTH, PADDLE_HEIGHT, COLOR_PADDLE);
  fill_rect(ball_x, ball_y, BALL_SIZE, BALL_SIZE, COLOR_BALL);

  /* Flush back-buffer → /dev/fb0 */
  lseek(fb_fd, 0, SEEK_SET);
  size_t total = (size_t)fb_pitch * fb_height, done = 0;
  while (done < total) {
    ssize_t n = write(fb_fd, fb_mem + done, total - done);
    if (n <= 0) break;
    done += (size_t)n;
  }
}

// ---------------------------------------------------------------------------
// Input — poll() to avoid blocking
// ---------------------------------------------------------------------------
static void handle_input(void) {
  if (use_x11) {
    while (XPending(x_display)) {
      XEvent event;
      XNextEvent(x_display, &event);
      if (event.type == ClientMessage &&
          (Atom)event.xclient.data.l[0] == wm_delete_window) {
        running = 0;
      } else if (event.type == KeyPress || event.type == KeyRelease) {
        int pressed = event.type == KeyPress;
        KeySym key = XLookupKeysym(&event.xkey, 0);
        if (key == XK_w || key == XK_W || key == XK_Up) move_up = pressed;
        if (key == XK_s || key == XK_S || key == XK_Down) move_down = pressed;
        if (pressed && (key == XK_q || key == XK_Q || key == XK_Escape))
          running = 0;
      }
    }
    if (move_up != move_down)
      player_y += move_down ? PADDLE_SPEED : -PADDLE_SPEED;
    if (player_y < 0) player_y = 0;
    if (player_y + PADDLE_HEIGHT > (int)fb_height)
      player_y = (int)fb_height - PADDLE_HEIGHT;
    return;
  }
  struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
  while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
    char buf[32];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < (int)n; i++) {
      if (i + 2 < (int)n && buf[i] == '\033' && buf[i+1] == '[') {
        switch (buf[i+2]) {
          case 'A': player_y -= PADDLE_SPEED; i += 2; break;
          case 'B': player_y += PADDLE_SPEED; i += 2; break;
        }
        continue;
      }
      switch (buf[i]) {
        case 'w': case 'W': player_y -= PADDLE_SPEED; break;
        case 's': case 'S': player_y += PADDLE_SPEED; break;
        case 'q': case 'Q': case '\033': cleanup(); _exit(0);
      }
    }
    pfd.revents = 0;
  }
  if (player_y < 0) player_y = 0;
  if (player_y + PADDLE_HEIGHT > (int)fb_height)
    player_y = (int)fb_height - PADDLE_HEIGHT;
}

// ---------------------------------------------------------------------------
// AI + physics
// ---------------------------------------------------------------------------
static void update_ai(void) {
  if (ball_vx < 0 || ball_x < (int)fb_width / 2) return;
  int ai_c = ai_y + PADDLE_HEIGHT / 2, bc = ball_y + BALL_SIZE / 2;
  if (ai_c < bc - 20)      ai_y += AI_SPEED;
  else if (ai_c > bc + 20) ai_y -= AI_SPEED;
  if (ai_y < 0) ai_y = 0;
  if (ai_y + PADDLE_HEIGHT > (int)fb_height) ai_y = (int)fb_height - PADDLE_HEIGHT;
}

static void reset_ball(void) {
  ball_x  = (int)fb_width  / 2 - BALL_SIZE / 2;
  ball_y  = (int)fb_height / 2 - BALL_SIZE / 2;
  ball_vx = (player_score + ai_score) % 2 == 0 ? BALL_SPEED : -BALL_SPEED;
  ball_vy = (player_score + ai_score) % 4 < 2 ? 3 : -3;
}

static void update_ball_step(int step_vx, int step_vy) {
  ball_x += step_vx;
  ball_y += step_vy;

  /* Walls */
  if (ball_y <= 0) {
    ball_y = 0; ball_vy = -ball_vy;
    sound_emit(SND_WALL_HIT);
  } else if (ball_y + BALL_SIZE >= (int)fb_height) {
    ball_y = (int)fb_height - BALL_SIZE; ball_vy = -ball_vy;
    sound_emit(SND_WALL_HIT);
  }

  /* Player paddle */
  int px = 30;
  if (ball_x <= px + PADDLE_WIDTH && ball_x + BALL_SIZE >= px &&
      ball_y + BALL_SIZE >= player_y && ball_y <= player_y + PADDLE_HEIGHT) {
    ball_vx = abs(ball_vx) + 2;
    ball_vy = ((ball_y + BALL_SIZE / 2) - player_y - PADDLE_HEIGHT / 2) / 4;
    ball_x  = px + PADDLE_WIDTH;
    sound_emit(SND_PADDLE_HIT);
  }

  /* AI paddle */
  int ax = (int)fb_width - 30 - PADDLE_WIDTH;
  if (ball_x + BALL_SIZE >= ax && ball_x <= ax + PADDLE_WIDTH &&
      ball_y + BALL_SIZE >= ai_y && ball_y <= ai_y + PADDLE_HEIGHT) {
    ball_vx = -(abs(ball_vx) + 2);
    ball_vy = ((ball_y + BALL_SIZE / 2) - ai_y - PADDLE_HEIGHT / 2) / 4;
    ball_x  = ax - BALL_SIZE;
    sound_emit(SND_PADDLE_HIT);
  }

  /* Speed cap */
  if (abs(ball_vx) > BALL_MAX_X)
    ball_vx = ball_vx > 0 ? BALL_MAX_X : -BALL_MAX_X;
  if (abs(ball_vy) > BALL_MAX_Y)
    ball_vy = ball_vy > 0 ? BALL_MAX_Y : -BALL_MAX_Y;

  /* Scoring */
  if (ball_x + BALL_SIZE < 0) {
    ai_score++;
    sound_emit(SND_SCORE);
    reset_ball();
  } else if (ball_x > (int)fb_width) {
    player_score++;
    sound_emit(SND_SCORE);
    reset_ball();
  }
}

static void update_ball(void) {
  /* Split fast movement into short segments so collisions stay reliable. */
  int distance = abs(ball_vx) > abs(ball_vy) ? abs(ball_vx) : abs(ball_vy);
  int steps = (distance + 3) / 4;
  if (steps < 1) steps = 1;

  int old_x = ball_x, old_y = ball_y;
  int target_x = old_x + ball_vx, target_y = old_y + ball_vy;
  for (int i = 1; i <= steps; i++) {
    int next_x = old_x + (target_x - old_x) * i / steps;
    int next_y = old_y + (target_y - old_y) * i / steps;
    int step_x = next_x - ball_x;
    int step_y = next_y - ball_y;
    update_ball_step(step_x, step_y);

    /* A hit or score changes velocity/position; stop following the stale path. */
    if ((ball_vx > 0) != (target_x > old_x) ||
        (ball_vy > 0) != (target_y > old_y) ||
        ball_x == (int)fb_width / 2 - BALL_SIZE / 2)
      break;
  }
}

// ---------------------------------------------------------------------------
// Delay
// ---------------------------------------------------------------------------
static int64_t monotonic_ns(void) {
  struct timespec ts;
  syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_until(int64_t deadline) {
  int64_t remaining = deadline - monotonic_ns();
  if (remaining <= 0) return;
  struct timespec ts = { remaining / 1000000000LL, remaining % 1000000000LL };
  syscall(SYS_nanosleep, &ts, NULL);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
  atexit(cleanup);
  signal(SIGINT,  sig_handler);
  signal(SIGTERM, sig_handler);
  signal(SIGHUP,  sig_handler);
  signal(SIGCHLD, SIG_DFL); /* don't reap sound child automatically */

  int force_fb = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-fb") == 0) force_fb = 1;
    else if (strcmp(argv[i], "-x11") == 0) force_fb = 0;
  }
  use_x11 = !force_fb && getenv("DISPLAY") && setup_x11() == 0;
  if (!use_x11) {
    if (setup_terminal() < 0) {
      fprintf(stderr, "pong: failed to set raw terminal mode\n");
      return 1;
    }
    if (setup_framebuffer() < 0) return 1;
    setup_tty();
  }
  setup_sound();

  player_y = (int)fb_height / 2 - PADDLE_HEIGHT / 2;
  ai_y     = (int)fb_height / 2 - PADDLE_HEIGHT / 2;
  reset_ball();

  const int64_t frame_ns = 1000000000LL / 60;
  int64_t next_frame = monotonic_ns();
  while (running) {
    handle_input();
    update_ai();
    update_ball();
    render();
    next_frame += frame_ns;
    sleep_until(next_frame);
    if (monotonic_ns() - next_frame > frame_ns * 4)
      next_frame = monotonic_ns();
  }
  return 0;
}
