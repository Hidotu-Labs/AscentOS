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
#define PADDLE_SPEED  8
#define BALL_SPEED    5
#define AI_SPEED      3

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
  tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);

  /* Kill sound child */
  if (snd_pipe[1] >= 0) { close(snd_pipe[1]); snd_pipe[1] = -1; }
  if (snd_pipe[0] >= 0) { close(snd_pipe[0]); snd_pipe[0] = -1; }
  if (snd_pid > 0) { kill(snd_pid, SIGTERM); waitpid(snd_pid, NULL, 0); snd_pid = -1; }

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
static inline void put_pixel(int x, int y, uint32_t color) {
  if (x < 0 || (uint32_t)x >= fb_width || y < 0 || (uint32_t)y >= fb_height) return;
  *(uint32_t *)(fb_mem + (uint32_t)y * fb_pitch + (uint32_t)x * 4) = color;
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
  for (int dy = 0; dy < h; dy++)
    for (int dx = 0; dx < w; dx++)
      put_pixel(x + dx, y + dy, color);
}

static void clear_screen(void) {
  uint32_t bg = COLOR_BG;
  for (uint32_t row = 0; row < fb_height; row++) {
    uint32_t *line = (uint32_t *)(fb_mem + row * fb_pitch);
    for (uint32_t col = 0; col < fb_width; col++)
      line[col] = bg;
  }
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

static void render(void) {
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
  ball_vy = 0;
}

static void update_ball(void) {
  ball_x += ball_vx;
  ball_y += ball_vy;

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
    ball_vx = abs(ball_vx) + 1;
    ball_vy = ((ball_y + BALL_SIZE / 2) - player_y - PADDLE_HEIGHT / 2) / 5;
    ball_x  = px + PADDLE_WIDTH;
    sound_emit(SND_PADDLE_HIT);
  }

  /* AI paddle */
  int ax = (int)fb_width - 30 - PADDLE_WIDTH;
  if (ball_x + BALL_SIZE >= ax && ball_x <= ax + PADDLE_WIDTH &&
      ball_y + BALL_SIZE >= ai_y && ball_y <= ai_y + PADDLE_HEIGHT) {
    ball_vx = -(abs(ball_vx) + 1);
    ball_vy = ((ball_y + BALL_SIZE / 2) - ai_y - PADDLE_HEIGHT / 2) / 5;
    ball_x  = ax - BALL_SIZE;
    sound_emit(SND_PADDLE_HIT);
  }

  /* Speed cap */
  if (abs(ball_vx) > 12) ball_vx = ball_vx > 0 ? 12 : -12;
  if (abs(ball_vy) > 8)  ball_vy = ball_vy > 0 ?  8 :  -8;

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

// ---------------------------------------------------------------------------
// Delay
// ---------------------------------------------------------------------------
static void delay_ms(int ms) {
  struct timespec ts = { 0, (long)ms * 1000000L };
  syscall(SYS_nanosleep, &ts, NULL);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void) {
  atexit(cleanup);
  signal(SIGINT,  sig_handler);
  signal(SIGTERM, sig_handler);
  signal(SIGHUP,  sig_handler);
  signal(SIGCHLD, SIG_DFL); /* don't reap sound child automatically */

  if (setup_terminal() < 0) {
    fprintf(stderr, "pong: failed to set raw terminal mode\n");
    return 1;
  }
  if (setup_framebuffer() < 0) return 1;
  setup_tty();
  setup_sound();

  player_y = (int)fb_height / 2 - PADDLE_HEIGHT / 2;
  ai_y     = (int)fb_height / 2 - PADDLE_HEIGHT / 2;
  reset_ball();

  for (;;) {
    handle_input();
    update_ai();
    update_ball();
    render();
    delay_ms(16);
  }
  return 0;
}
