#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>

/* OSS /dev/dsp ioctls */
#define SNDCTL_DSP_SPEED    0xC0045002
#define SNDCTL_DSP_STEREO   0xC0045003
#define SNDCTL_DSP_SETFMT   0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define AFMT_U8             0x00000008
#define AFMT_S16_LE         0x00000010

#pragma pack(push, 1)
typedef struct {
    char     riff_id[4];
    uint32_t riff_size;
    char     wave_id[4];
} WavHeader;

typedef struct {
    char     chunk_id[4];
    uint32_t chunk_size;
} ChunkHeader;

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FmtChunk;
#pragma pack(pop)

int main(int argc, char *argv[]) {
    const char *path = (argc >= 2) ? argv[1] : "/boot.wav";

    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;

    WavHeader header;
    if (read(fd, &header, sizeof(WavHeader)) != sizeof(WavHeader)) {
        close(fd); return 1;
    }
    if (strncmp(header.riff_id, "RIFF", 4) != 0 ||
        strncmp(header.wave_id, "WAVE", 4) != 0) {
        close(fd); return 1;
    }

    ChunkHeader chunk_hdr;
    FmtChunk    fmt;
    int         fmt_found = 0;
    uint32_t    data_size = 0;

    while (read(fd, &chunk_hdr, sizeof(ChunkHeader)) == sizeof(ChunkHeader)) {
        if (strncmp(chunk_hdr.chunk_id, "fmt ", 4) == 0) {
            read(fd, &fmt, sizeof(FmtChunk));
            if (chunk_hdr.chunk_size > sizeof(FmtChunk))
                lseek(fd, chunk_hdr.chunk_size - sizeof(FmtChunk), SEEK_CUR);
            fmt_found = 1;
        } else if (strncmp(chunk_hdr.chunk_id, "data", 4) == 0) {
            data_size = chunk_hdr.chunk_size;
            break;
        } else {
            lseek(fd, chunk_hdr.chunk_size, SEEK_CUR);
        }
    }

    if (!fmt_found || data_size == 0) { close(fd); return 1; }

    if (fmt.audio_format != 1 ||
        (fmt.num_channels != 1 && fmt.num_channels != 2) ||
        (fmt.bits_per_sample != 8 && fmt.bits_per_sample != 16)) {
        close(fd); return 1;
    }

    int dsp_fd = open("/dev/dsp", O_WRONLY);
    if (dsp_fd < 0) { close(fd); return 1; }

    int sample_rate = (int)fmt.sample_rate;
    ioctl(dsp_fd, SNDCTL_DSP_SPEED, &sample_rate);

    int channels = (int)fmt.num_channels;
    ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &channels);

    int format = (fmt.bits_per_sample == 16) ? AFMT_S16_LE : AFMT_U8;
    ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &format);

    const uint32_t CHUNK = 65536;
    uint8_t *buffer = malloc(CHUNK);
    if (!buffer) { close(dsp_fd); close(fd); return 1; }

    int bytes_read;
    while ((bytes_read = read(fd, buffer, CHUNK)) > 0) {
        int total_written = 0;
        while (total_written < bytes_read) {
            int written = write(dsp_fd, buffer + total_written,
                                bytes_read - total_written);
            if (written < 0) goto done;
            if (written == 0) {
                struct timespec ts = {0, 10000000}; /* 10 ms */
                syscall(SYS_nanosleep, &ts, NULL);
            } else {
                total_written += written;
            }
        }
    }

done:
    free(buffer);
    close(dsp_fd);
    close(fd);
    return 0;
}
