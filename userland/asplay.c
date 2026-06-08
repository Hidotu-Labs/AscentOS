#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>

// OSS /dev/dsp ioctls
#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_STEREO 0xC0045003
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006

#define AFMT_U8 0x00000008
#define AFMT_S16_LE 0x00000010

enum {
  ARROW_UP = 1000,
  ARROW_DOWN,
  KEY_ENTER = 13,
  KEY_ESC = 27
};

typedef struct {
  char **paths;
  int count;
  int current;
} playlist_t;

typedef enum {
  MODE_MENU,
  MODE_PLAYING
} app_mode_t;

playlist_t playlist;
app_mode_t app_mode = MODE_MENU;
int menu_selection = 0;
int screen_rows, screen_cols;
int should_exit = 0;
int skip_track = 0; // 1 for next, -1 for prev
struct termios orig_termios;
int raw_mode_enabled = 0;

typedef struct {
  char riff_id[4];
  uint32_t riff_size;
  char wave_id[4];
} __attribute__((packed)) WavHeader;

typedef struct {
  char chunk_id[4];
  uint32_t chunk_size;
} __attribute__((packed)) ChunkHeader;

typedef struct {
  uint16_t audio_format;
  uint16_t num_channels;
  uint32_t sample_rate;
  uint32_t byte_rate;
  uint16_t block_align;
  uint16_t bits_per_sample;
} __attribute__((packed)) FmtChunk;

void disable_raw_mode() {
  if (raw_mode_enabled) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
  }
}

void enable_raw_mode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    return;
  atexit(disable_raw_mode);
  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  raw_mode_enabled = 1;
  printf("\033[?25l"); // Hide cursor
}

int get_window_size(int *rows, int *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    *rows = 24;
    *cols = 80;
    return -1;
  }
  *rows = ws.ws_row;
  *cols = ws.ws_col;
  return 0;
}

int read_key() {
  struct pollfd pfd;
  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;
  // Use timeout 0 to be non-blocking
  if (poll(&pfd, 1, 0) <= 0) return 0;

  char c;
  int nread = read(STDIN_FILENO, &c, 1);
  if (nread == -1 && errno != EAGAIN)
    return -1;
  if (nread != 1)
    return 0;

  if (c == '\033') {
    char seq[2];
    struct timespec ts = {0, 1000000}; // 1ms
    syscall(SYS_nanosleep, &ts, NULL);
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\033';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\033';
    if (seq[0] == '[') {
      switch (seq[1]) {
      case 'A': return ARROW_UP;
      case 'B': return ARROW_DOWN;
      }
    }
    return '\033';
  }
  return c;
}

void scan_music() {
  DIR *d = opendir(".");
  if (!d) return;
  struct dirent *dir;
  while ((dir = readdir(d)) != NULL) {
    if (strstr(dir->d_name, ".mp3") || strstr(dir->d_name, ".wav") ||
        strstr(dir->d_name, ".MP3") || strstr(dir->d_name, ".WAV")) {
      playlist.paths = realloc(playlist.paths, sizeof(char *) * (playlist.count + 1));
      playlist.paths[playlist.count++] = strdup(dir->d_name);
    }
  }
  closedir(d);
}

void draw_menu() {
  get_window_size(&screen_rows, &screen_cols);
  printf("\033[H");
  printf("\033[1;36mAscentOS Audio Player\033[0m\033[K\n\033[K\n");

  int list_height = screen_rows - 6;
  if (list_height < 1) list_height = 1;
  int start = (menu_selection > list_height / 2) ? menu_selection - list_height / 2 : 0;

  for (int i = 0; i < list_height; i++) {
    int idx = start + i;
    if (idx < playlist.count) {
      if (idx == menu_selection) {
        printf(" \033[1;32m-> %s\033[0m\033[K\n", playlist.paths[idx]);
      } else {
        printf("    %s\033[K\n", playlist.paths[idx]);
      }
    } else {
      printf("\033[K\n");
    }
  }
  printf("\n\033[1m[UP/DOWN]\033[0m Select, \033[1m[ENTER]\033[0m Play, \033[1m[Q]\033[0m Quit\033[K");
  printf("\033[J");
  fflush(stdout);
}

void draw_tui(const char *current_track, double progress) {
  static clock_t last_draw = 0;
  clock_t now = clock();
  if (now - last_draw < CLOCKS_PER_SEC / 10 && progress < 0.99) return;
  last_draw = now;

  get_window_size(&screen_rows, &screen_cols);
  printf("\033[H"); // Home

  // Header
  printf("\033[1;36mAscentOS Audio Player [PLAYING]\033[0m\033[K\n\033[K\n");

  // Progress bar
  printf("\033[1mNow Playing: %s\033[0m\033[K\n\033[K", current_track);
  int bar_width = screen_cols - 12;
  if (bar_width < 10) bar_width = 10;
  int pos = (int)(progress * bar_width);
  if (pos > bar_width) pos = bar_width;

  printf(" [");
  for (int i = 0; i < bar_width; i++) {
    if (i < pos) printf("=");
    else if (i == pos) printf(">");
    else printf(" ");
  }
  printf("] %3d%%\033[K\n\033[K", (int)(progress * 100));

  // Controls
  printf("\nControls: \033[1m[N]\033[0m Next, \033[1m[P]\033[0m Prev, \033[1m[M]\033[0m Menu, \033[1m[Q]\033[0m Quit\033[K\n");
  printf("\033[J"); // Clear remaining screen once
  fflush(stdout);
}

void handle_input() {
  int key = read_key();
  if (key == 0) return;

  if (app_mode == MODE_MENU) {
    if (key == ARROW_UP) {
      if (menu_selection > 0) menu_selection--;
    } else if (key == ARROW_DOWN) {
      if (menu_selection < playlist.count - 1) menu_selection++;
    } else if (key == KEY_ENTER || key == 10) {
      playlist.current = menu_selection;
      app_mode = MODE_PLAYING;
    } else if (tolower(key) == 'q') {
      should_exit = 1;
    }
  } else {
    if (tolower(key) == 'q') {
      should_exit = 1;
      skip_track = 1;
    } else if (tolower(key) == 'n') {
      skip_track = 1;
    } else if (tolower(key) == 'p') {
      skip_track = -1;
    } else if (tolower(key) == 'm') {
      skip_track = 1;
      app_mode = MODE_MENU;
    }
  }
}

int open_dsp() {
  int dsp_fd = open("/dev/dsp", O_WRONLY);
  if (dsp_fd < 0)
    dsp_fd = open("/dev/hda_audio", O_WRONLY);
  if (dsp_fd < 0)
    dsp_fd = open("/dev/ac97", O_WRONLY);
  return dsp_fd;
}

int write_to_dsp(int dsp_fd, void *buffer, int bytes) {
  int total_written = 0;
  while (total_written < bytes) {
    int written = write(dsp_fd, (uint8_t *)buffer + total_written, bytes - total_written);
    if (written < 0) {
      return -1;
    }
    if (written == 0) {
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = 10000000; // 10ms
      syscall(SYS_nanosleep, &ts, NULL);
    } else {
      total_written += written;
    }
  }
  return total_written;
}

int play_wav(int fd, const char *filename) {
  WavHeader header;
  lseek(fd, 0, SEEK_SET);
  if (read(fd, &header, sizeof(WavHeader)) != sizeof(WavHeader)) {
    return -1;
  }

  if (strncmp(header.riff_id, "RIFF", 4) != 0 ||
      strncmp(header.wave_id, "WAVE", 4) != 0) {
    return -1;
  }

  ChunkHeader chunk_hdr;
  FmtChunk fmt;
  int fmt_found = 0;
  uint32_t data_size = 0;

  while (read(fd, &chunk_hdr, sizeof(ChunkHeader)) == sizeof(ChunkHeader)) {
    if (strncmp(chunk_hdr.chunk_id, "fmt ", 4) == 0) {
      read(fd, &fmt, sizeof(FmtChunk));
      if (chunk_hdr.chunk_size > sizeof(FmtChunk)) {
        lseek(fd, chunk_hdr.chunk_size - sizeof(FmtChunk), SEEK_CUR);
      }
      fmt_found = 1;
    } else if (strncmp(chunk_hdr.chunk_id, "data", 4) == 0) {
      data_size = chunk_hdr.chunk_size;
      break;
    } else {
      lseek(fd, chunk_hdr.chunk_size, SEEK_CUR);
    }
  }

  if (!fmt_found || data_size == 0) {
    printf("playwav: invalid WAV format in %s\n", filename);
    return 1;
  }

  if (fmt.audio_format != 1 ||
      (fmt.num_channels != 1 && fmt.num_channels != 2) ||
      (fmt.bits_per_sample != 8 && fmt.bits_per_sample != 16)) {
    printf("playwav: unsupported WAV format: fmt=%d, ch=%d, bits=%d\n",
           fmt.audio_format, fmt.num_channels, fmt.bits_per_sample);
    return 1;
  }

  // printf("Playing WAV: %s (%d Hz, %d channels, %d bits)\n", filename,
  //        fmt.sample_rate, fmt.num_channels, fmt.bits_per_sample);

  int dsp_fd = open_dsp();
  if (dsp_fd < 0) {
    printf("playwav: could not open audio device\n");
    return 1;
  }

  int sample_rate = (int)fmt.sample_rate;
  ioctl(dsp_fd, SNDCTL_DSP_SPEED, &sample_rate);
  int channels = (int)fmt.num_channels;
  ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &channels);
  int format = (fmt.bits_per_sample == 16) ? AFMT_S16_LE : AFMT_U8;
  ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &format);

  uint32_t chunk_size = 65536; // Larger chunks for smoother WAV playback
  uint8_t *buffer = malloc(chunk_size);
  int bytes_read;
  uint32_t played_bytes = 0;
  while (played_bytes < data_size) {
    uint32_t to_read = chunk_size;
    if (to_read > data_size - played_bytes) to_read = data_size - played_bytes;
    
    bytes_read = read(fd, buffer, to_read);
    if (bytes_read <= 0) break;
    
    if (write_to_dsp(dsp_fd, buffer, bytes_read) < 0) break;
    played_bytes += bytes_read;
    handle_input();
    if (skip_track) break;
    draw_tui(filename, (double)played_bytes / data_size);
  }

  free(buffer);
  close(dsp_fd);
  return 0;
}

int play_mp3(int fd, const char *filename) {
  // Read entire file into memory for simplicity with minimp3
  off_t file_size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  uint8_t *mp3_data = malloc(file_size);
  if (!mp3_data) {
    printf("playwav: out of memory for MP3\n");
    return 1;
  }

  if (read(fd, mp3_data, file_size) != file_size) {
    printf("playwav: failed to read %s\n", filename);
    free(mp3_data);
    return 1;
  }

  mp3dec_t mp3d;
  mp3dec_init(&mp3d);
  mp3dec_frame_info_t info;
  int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

  int dsp_fd = -1;
  int current_rate = 0;
  int current_channels = 0;

  int data_left = file_size;
  uint8_t *ptr = mp3_data;

  while (data_left > 0) {
    memset(&info, 0, sizeof(info));
    int samples = mp3dec_decode_frame(&mp3d, ptr, data_left, pcm, &info);
    
    if (samples > 0) {
      if (dsp_fd < 0) {
        dsp_fd = open_dsp();
        if (dsp_fd < 0) {
          free(mp3_data);
          return 1;
        }
      }

      if (info.hz != current_rate || info.channels != current_channels) {
        int rate = info.hz;
        ioctl(dsp_fd, SNDCTL_DSP_SPEED, &rate);
        int ch = info.channels;
        ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &ch);
        int format = AFMT_S16_LE;
        ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &format);
        current_rate = info.hz;
        current_channels = info.channels;
      }
      if (write_to_dsp(dsp_fd, pcm, samples * info.channels * 2) < 0) break;
    }

    handle_input();
    if (skip_track) break;
    draw_tui(filename, (double)(ptr - mp3_data) / file_size);
    
    if (info.frame_bytes == 0) {
        ptr++;
        data_left--;
        continue;
    }

    ptr += info.frame_bytes;
    data_left -= info.frame_bytes;
  }

  if (dsp_fd >= 0) close(dsp_fd);
  free(mp3_data);
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc > 1) {
    playlist.paths = malloc(sizeof(char *) * (argc - 1));
    playlist.count = 0;
    for (int i = 1; i < argc; i++) {
      playlist.paths[playlist.count++] = strdup(argv[i]);
    }
  } else {
    scan_music();
  }

  if (playlist.count == 0) {
    printf("No music files found (.mp3 or .wav)\n");
    return 1;
  }

  playlist.current = 0;
  enable_raw_mode();
  printf("\033[2J"); // Clear screen

  while (!should_exit) {
    if (app_mode == MODE_MENU) {
      draw_menu();
      handle_input();
      struct timespec ts;
      ts.tv_sec = 0;
      ts.tv_nsec = 30000000; // 30ms sleep for responsiveness
      syscall(SYS_nanosleep, &ts, NULL);
    } else {
      const char *filename = playlist.paths[playlist.current];
      int fd = open(filename, O_RDONLY);
      if (fd < 0) {
        app_mode = MODE_MENU;
        continue;
      }

      skip_track = 0;
      int res = play_wav(fd, filename);
      if (res == -1) {
        // Check extension as fallback or just try MP3
        play_mp3(fd, filename);
      }
      close(fd);

      if (should_exit) break;

      if (skip_track == -1) {
        playlist.current--;
        if (playlist.current < 0) playlist.current = playlist.count - 1;
      } else if (skip_track == 1) {
        // Only advance if we are still in playing mode (not switched to menu)
        if (app_mode == MODE_PLAYING) {
          playlist.current++;
          if (playlist.current >= playlist.count) playlist.current = 0;
        }
      } else {
        // Naturally finished, return to menu
        app_mode = MODE_MENU;
      }
    }
  }

  disable_raw_mode();
  printf("\033[2J\033[H");
  return 0;
}
