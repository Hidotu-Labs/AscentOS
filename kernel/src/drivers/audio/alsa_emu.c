#include "alsa_emu.h"
#include "audio_dsp.h"
#include "hda.h"
#include "../../fb/framebuffer.h"
#include "../../fs/vfs.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include <stdbool.h>
#include <stdint.h>

// ALSA Protocol Versions
#define ALSA_CTL_VERSION 0x00020008
#define ALSA_PCM_VERSION 0x0002000F

// ALSA Control IOCTLs
#define SNDRV_CTL_IOCTL_PVERSION        0x80045500
#define SNDRV_CTL_IOCTL_CARD_INFO       0x81785501
#define SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE 0xC0045530
#define SNDRV_CTL_IOCTL_PCM_INFO        0xC1205531

// ALSA PCM IOCTLs
#define SNDRV_PCM_IOCTL_PVERSION        0x80044100
#define SNDRV_PCM_IOCTL_INFO            0x81204101
#define SNDRV_PCM_IOCTL_TSTAMP          0x40044102
#define SNDRV_PCM_IOCTL_TTSTAMP         0x40044103
#define SNDRV_PCM_IOCTL_HW_REFINE       0xC2604110
#define SNDRV_PCM_IOCTL_HW_PARAMS       0xC2604111
#define SNDRV_PCM_IOCTL_HW_FREE         0x00004112
#define SNDRV_PCM_IOCTL_SW_PARAMS       0xC0684113
#define SNDRV_PCM_IOCTL_STATUS          0x80984120
#define SNDRV_PCM_IOCTL_DELAY           0x80084121
#define SNDRV_PCM_IOCTL_HWSYNC          0x00004122
#define SNDRV_PCM_IOCTL_SYNC_PTR        0xC0884123
#define SNDRV_PCM_IOCTL_CHANNEL_INFO    0x80184132
#define SNDRV_PCM_IOCTL_PREPARE         0x00004140
#define SNDRV_PCM_IOCTL_RESET           0x00004141
#define SNDRV_PCM_IOCTL_START           0x00004142
#define SNDRV_PCM_IOCTL_DROP            0x00004143
#define SNDRV_PCM_IOCTL_DRAIN           0x00004144
#define SNDRV_PCM_IOCTL_PAUSE           0x40044145
#define SNDRV_PCM_IOCTL_WRITEI_FRAMES   0x40184150
#define SNDRV_PCM_IOCTL_READI_FRAMES    0x80184151

// ALSA HW_PARAM Indices
#define SNDRV_PCM_HW_PARAM_ACCESS       0
#define SNDRV_PCM_HW_PARAM_FORMAT       1
#define SNDRV_PCM_HW_PARAM_SUBFORMAT    2

#define SNDRV_PCM_HW_PARAM_SAMPLE_BITS  8
#define SNDRV_PCM_HW_PARAM_FRAME_BITS   9
#define SNDRV_PCM_HW_PARAM_CHANNELS     10
#define SNDRV_PCM_HW_PARAM_RATE         11
#define SNDRV_PCM_HW_PARAM_PERIOD_TIME  12
#define SNDRV_PCM_HW_PARAM_PERIOD_SIZE  13
#define SNDRV_PCM_HW_PARAM_PERIOD_BYTES 14
#define SNDRV_PCM_HW_PARAM_PERIODS      15
#define SNDRV_PCM_HW_PARAM_BUFFER_TIME  16
#define SNDRV_PCM_HW_PARAM_BUFFER_SIZE  17
#define SNDRV_PCM_HW_PARAM_BUFFER_BYTES 18
#define SNDRV_PCM_HW_PARAM_TICK_TIME    19

// Data Structures
struct snd_ctl_card_info {
    int card;
    int pad;
    uint8_t id[16];
    uint8_t driver[16];
    uint8_t name[32];
    uint8_t longname[80];
    uint8_t mixername[80];
    uint8_t components[128];
};

struct snd_pcm_info {
    uint32_t device;
    uint32_t subdevice;
    int32_t stream;
    int32_t card;
    uint8_t id[64];
    uint8_t name[80];
    uint8_t subname[32];
    int32_t dev_class;
    int32_t dev_subclass;
    uint32_t subdevices_count;
    uint32_t subdevices_avail;
    uint8_t sync[16];
    uint8_t reserved[64];
};

struct snd_interval {
    uint32_t min, max;
    uint32_t openmin:1,
             openmax:1,
             integer:1,
             empty:1;
};

struct snd_mask {
    uint32_t bits[8];
};

struct snd_pcm_hw_params {
    uint32_t flags;
    struct snd_mask masks[3];
    struct snd_mask mres[5];
    struct snd_interval intervals[12];
    struct snd_interval ires[9];
    uint32_t rmask;
    uint32_t cmask;
    uint32_t info;
    uint32_t msbits;
    uint32_t rate_num;
    uint32_t rate_den;
    uint64_t fifo_size;
    uint8_t reserved[64];
};

struct snd_pcm_status {
    int32_t state;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } trigger_tstamp;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } tstamp;
    uint64_t appl_ptr;
    uint64_t hw_ptr;
    int64_t delay;
    uint64_t avail;
    uint64_t avail_max;
    uint64_t overrange;
    int32_t suspended_state;
    uint32_t audio_tstamp_data;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } audio_tstamp;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } driver_tstamp;
    uint32_t audio_tstamp_accuracy;
    uint8_t reserved[52 - 32];
};

struct snd_pcm_mmap_status {
    int32_t state;
    int32_t pad1;
    uint64_t hw_ptr;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } tstamp;
    int32_t suspended_state;
    struct {
        uint64_t tv_sec;
        uint64_t tv_nsec;
    } audio_tstamp;
};

struct snd_pcm_mmap_control {
    uint64_t appl_ptr;
    uint64_t avail_min;
};

struct snd_pcm_sync_ptr {
    uint32_t flags;
    union {
        struct snd_pcm_mmap_status status;
        uint8_t reserved[64];
    } s;
    union {
        struct snd_pcm_mmap_control control;
        uint8_t reserved[64];
    } c;
};

struct snd_pcm_channel_info {
    uint32_t channel;
    uint64_t offset;
    uint32_t first;
    uint32_t step;
};

struct snd_xferi {
    int64_t result;
    void *buf;
    uint64_t frames;
};

// Internal ALSA state
static uint32_t alsa_sample_rate = 44100;
static uint8_t alsa_channels = 2;
static uint8_t alsa_bits = 16;
static int alsa_pcm_state = 0; // 0 = OPEN, 1 = PREPARED, 2 = RUNNING
static uint64_t alsa_appl_ptr = 0;
static uint64_t alsa_hw_ptr = 0;

// ALSA Control Node Callbacks
static int alsa_ctl_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
    (void)node;
    if (request == SNDRV_CTL_IOCTL_PVERSION) {
        int *ver = (int *)arg;
        if (!ver) return -14;
        *ver = ALSA_CTL_VERSION;
        return 0;
    }

    if (request == SNDRV_CTL_IOCTL_CARD_INFO) {
        struct snd_ctl_card_info *info = (struct snd_ctl_card_info *)arg;
        if (!info) return -14;
        memset(info, 0, sizeof(*info));
        info->card = 0;
        strcpy((char *)info->id, "HDA");
        strcpy((char *)info->driver, "HDA-Intel");
        strcpy((char *)info->name, "HDA Intel");
        strcpy((char *)info->longname, "HDA Intel Audio Controller (AvoryOS)");
        strcpy((char *)info->mixername, "Generic HDA Codec");
        strcpy((char *)info->components, "HDA:00000000");
        return 0;
    }

    if (request == SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE) {
        int *dev = (int *)arg;
        if (!dev) return -14;
        if (*dev < 0) {
            *dev = 0;
        } else {
            *dev = -1;
        }
        return 0;
    }

    if (request == SNDRV_CTL_IOCTL_PCM_INFO) {
        struct snd_pcm_info *info = (struct snd_pcm_info *)arg;
        if (!info) return -14;
        memset(info, 0, sizeof(*info));
        info->device = 0;
        info->subdevice = 0;
        info->stream = 0;
        info->card = 0;
        strcpy((char *)info->id, "HDA PCM");
        strcpy((char *)info->name, "HDA Intel PCM");
        strcpy((char *)info->subname, "subdevice #0");
        info->subdevices_count = 1;
        info->subdevices_avail = 1;
        return 0;
    }

    return 0;
}

// ALSA PCM Node Callbacks
static int alsa_pcm_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
    (void)node;

    if (request == SNDRV_PCM_IOCTL_PVERSION) {
        int *ver = (int *)arg;
        if (!ver) return -14;
        *ver = ALSA_PCM_VERSION;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_INFO) {
        struct snd_pcm_info *info = (struct snd_pcm_info *)arg;
        if (!info) return -14;
        memset(info, 0, sizeof(*info));
        info->device = 0;
        info->subdevice = 0;
        info->stream = 0; // Playback
        info->card = 0;
        strcpy((char *)info->id, "HDA PCM");
        strcpy((char *)info->name, "HDA Intel PCM");
        strcpy((char *)info->subname, "subdevice #0");
        info->subdevices_count = 1;
        info->subdevices_avail = 1;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_HW_REFINE || request == SNDRV_PCM_IOCTL_HW_PARAMS) {
        struct snd_pcm_hw_params *params = (struct snd_pcm_hw_params *)arg;
        if (!params) return -14;

        uint32_t supported_access = (1U << 2) | (1U << 3); // Strictly RW_INTERLEAVED, RW_NONINTERLEAVED (No hardware MMAP)
        uint32_t supported_format = (1U << 2); // Strictly SNDRV_PCM_FORMAT_S16_LE
        uint32_t supported_subformat = (1U << 0); // STD

        if (params->masks[0].bits[0]) {
            params->masks[0].bits[0] &= supported_access;
            if (!params->masks[0].bits[0]) params->masks[0].bits[0] = (1U << 2);
        } else {
            params->masks[0].bits[0] = supported_access;
        }

        if (params->masks[1].bits[0]) {
            params->masks[1].bits[0] &= supported_format;
            if (!params->masks[1].bits[0]) params->masks[1].bits[0] = (1U << 2);
        } else {
            params->masks[1].bits[0] = supported_format;
        }

        if (params->masks[2].bits[0]) {
            params->masks[2].bits[0] &= supported_subformat;
            if (!params->masks[2].bits[0]) params->masks[2].bits[0] = (1U << 0);
        } else {
            params->masks[2].bits[0] = supported_subformat;
        }

        for (int i = 0; i < 12; i++) {
            params->intervals[i].openmin = 0;
            params->intervals[i].openmax = 0;
            params->intervals[i].integer = 1;
            params->intervals[i].empty = 0;
        }

        // intervals[0] = SAMPLE_BITS (16)
        params->intervals[0].min = 16;
        params->intervals[0].max = 16;
        // intervals[1] = FRAME_BITS (32)
        params->intervals[1].min = 32;
        params->intervals[1].max = 32;
        // intervals[2] = CHANNELS (2)
        params->intervals[2].min = 2;
        params->intervals[2].max = 2;
        // intervals[3] = RATE (8000..192000)
        if (params->intervals[3].min < 8000) params->intervals[3].min = 8000;
        if (params->intervals[3].max == 0 || params->intervals[3].max > 192000) params->intervals[3].max = 192000;
        // intervals[4] = PERIOD_TIME (10000..1000000 us)
        if (params->intervals[4].min < 10000) params->intervals[4].min = 10000;
        if (params->intervals[4].max == 0 || params->intervals[4].max > 1000000) params->intervals[4].max = 1000000;
        // intervals[5] = PERIOD_SIZE (1024..8192 frames)
        if (params->intervals[5].min < 1024) params->intervals[5].min = 1024;
        if (params->intervals[5].max == 0 || params->intervals[5].max > 8192) params->intervals[5].max = 8192;
        // intervals[6] = PERIOD_BYTES (4096..32768 bytes)
        if (params->intervals[6].min < 4096) params->intervals[6].min = 4096;
        if (params->intervals[6].max == 0 || params->intervals[6].max > 32768) params->intervals[6].max = 32768;
        // intervals[7] = PERIODS (2..8)
        if (params->intervals[7].min < 2) params->intervals[7].min = 2;
        if (params->intervals[7].max == 0 || params->intervals[7].max > 8) params->intervals[7].max = 8;
        // intervals[8] = BUFFER_TIME (20000..2000000 us)
        if (params->intervals[8].min < 20000) params->intervals[8].min = 20000;
        if (params->intervals[8].max == 0 || params->intervals[8].max > 2000000) params->intervals[8].max = 2000000;
        // intervals[9] = BUFFER_SIZE (4096..65536 frames)
        if (params->intervals[9].min < 4096) params->intervals[9].min = 4096;
        if (params->intervals[9].max == 0 || params->intervals[9].max > 65536) params->intervals[9].max = 65536;
        // intervals[10] = BUFFER_BYTES (16384..262144 bytes)
        if (params->intervals[10].min < 16384) params->intervals[10].min = 16384;
        if (params->intervals[10].max == 0 || params->intervals[10].max > 262144) params->intervals[10].max = 262144;

        params->rmask = 0;
        params->cmask = 0;
        params->info = 0x00000001 | 0x00000002 | 0x00000008 | 0x00000010; // NONINTERLEAVED | BLOCK_TRANSFER | RESUME
        params->rate_num = alsa_sample_rate;
        params->rate_den = 1;
        params->fifo_size = 0;

        if (request == SNDRV_PCM_IOCTL_HW_PARAMS) {
            // Collapse chosen masks to single bit
            if (params->masks[0].bits[0]) {
                uint32_t b = params->masks[0].bits[0];
                params->masks[0].bits[0] = (b & (b - 1)) ? (1U << 2) : b; // default RW_INTERLEAVED
            }
            if (params->masks[1].bits[0]) {
                uint32_t b = params->masks[1].bits[0];
                params->masks[1].bits[0] = (b & (b - 1)) ? (1U << 2) : b; // default S16_LE
            }
            // Collapse intervals to exact values
            for (int i = 0; i < 12; i++) {
                if (params->intervals[i].min) {
                    params->intervals[i].max = params->intervals[i].min;
                }
            }

            if (params->intervals[3].min >= 8000 && params->intervals[3].min <= 192000) {
                alsa_sample_rate = params->intervals[3].min;
            }
            if (params->intervals[2].min >= 1 && params->intervals[2].min <= 2) {
                alsa_channels = (uint8_t)params->intervals[2].min;
            }
            if (hda_is_present()) {
                hda_reset_stream();
            }
            alsa_pcm_state = 1; // PREPARED
            alsa_appl_ptr = 0;
            alsa_hw_ptr = 0;
        }
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_SW_PARAMS) {
        struct {
            int tstamp_mode;
            unsigned int period_step;
            unsigned int sleep_min;
            uint64_t avail_min;
            uint64_t xfer_align;
            uint64_t start_threshold;
            uint64_t stop_threshold;
            uint64_t silence_threshold;
            uint64_t silence_size;
            uint64_t boundary;
            unsigned int proto;
            unsigned int tstamp_type;
            unsigned char reserved[56];
        } *sw = (void *)arg;
        if (sw) {
            sw->boundary = 0x7FFFFFFFFFFFFFFFULL;
        }
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_CHANNEL_INFO) {
        struct snd_pcm_channel_info *ch = (struct snd_pcm_channel_info *)arg;
        if (!ch) return -14;
        ch->offset = ch->channel * 2;
        ch->first = ch->channel * 16;
        ch->step = 32;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_DELAY) {
        int64_t *delay = (int64_t *)arg;
        if (!delay) return -14;
        uint32_t frame_size = (alsa_bits / 8) * alsa_channels;
        if (frame_size == 0) frame_size = 4;
        uint32_t queued = hda_is_present() ? hda_get_ring_count() : 0;
        uint64_t queued_frames = queued / frame_size;
        if (queued_frames > alsa_appl_ptr) queued_frames = alsa_appl_ptr;
        *delay = (int64_t)queued_frames;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_STATUS) {
        struct snd_pcm_status *st = (struct snd_pcm_status *)arg;
        if (!st) return -14;
        memset(st, 0, sizeof(*st));
        st->state = (alsa_pcm_state == 2) ? 3 : 2; // 3 = RUNNING, 2 = PREPARED
        uint32_t frame_size = (alsa_bits / 8) * alsa_channels;
        if (frame_size == 0) frame_size = 4;
        uint32_t queued_bytes = hda_is_present() ? hda_get_ring_count() : 0;
        uint64_t queued_frames = queued_bytes / frame_size;
        if (queued_frames > alsa_appl_ptr) queued_frames = alsa_appl_ptr;
        st->appl_ptr = alsa_appl_ptr;
        st->hw_ptr = alsa_appl_ptr - queued_frames;
        st->delay = (int64_t)queued_frames;
        uint32_t avail_bytes = (524288 > queued_bytes) ? (524288 - queued_bytes) : 0;
        st->avail = avail_bytes / frame_size;
        st->avail_max = 524288 / frame_size;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_SYNC_PTR) {
        struct snd_pcm_sync_ptr *sync = (struct snd_pcm_sync_ptr *)arg;
        if (!sync) return -14;
        sync->s.status.state = (alsa_pcm_state == 2) ? 3 : 2;
        alsa_appl_ptr = sync->c.control.appl_ptr;
        uint32_t frame_size = (alsa_bits / 8) * alsa_channels;
        if (frame_size == 0) frame_size = 4;
        uint32_t queued_bytes = hda_is_present() ? hda_get_ring_count() : 0;
        uint64_t queued_frames = queued_bytes / frame_size;
        if (queued_frames > alsa_appl_ptr) queued_frames = alsa_appl_ptr;
        sync->s.status.hw_ptr = alsa_appl_ptr - queued_frames;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_PREPARE) {
        if (hda_is_present()) {
            hda_reset_stream();
        }
        alsa_pcm_state = 1; // PREPARED
        alsa_appl_ptr = 0;
        alsa_hw_ptr = 0;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_START) {
        alsa_pcm_state = 2; // RUNNING
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_DROP || request == SNDRV_PCM_IOCTL_RESET) {
        if (hda_is_present()) {
            hda_reset_stream();
        }
        alsa_pcm_state = 1; // PREPARED
        alsa_appl_ptr = 0;
        alsa_hw_ptr = 0;
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_DRAIN) {
        return 0;
    }

    if (request == SNDRV_PCM_IOCTL_WRITEI_FRAMES) {
        struct snd_xferi *xferi = (struct snd_xferi *)arg;
        if (!xferi || !xferi->buf) return -14;

        uint32_t frame_size = (alsa_bits / 8) * alsa_channels;
        uint32_t total_bytes = (uint32_t)(xferi->frames * frame_size);

        uint32_t written = 0;
        if (hda_is_present()) {
            written = hda_write_pcm(xferi->buf, total_bytes, alsa_sample_rate, alsa_channels, alsa_bits);
        } else {
            vfs_node_t *dsp = fb_lookup_device("dsp");
            if (dsp && dsp->write) {
                written = dsp->write(dsp, 0, total_bytes, (uint8_t *)xferi->buf);
            }
        }

        uint64_t frames_written = (written / frame_size);
        xferi->result = frames_written;
        alsa_appl_ptr += frames_written;
        alsa_pcm_state = 2; // RUNNING
        return 0;
    }

    return 0;
}

static uint32_t alsa_pcm_write(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
    (void)node;
    (void)offset;
    if (hda_is_present()) {
        return hda_write_pcm(buffer, size, alsa_sample_rate, alsa_channels, alsa_bits);
    }
    vfs_node_t *dsp = fb_lookup_device("dsp");
    if (dsp && dsp->write) {
        return dsp->write(dsp, offset, size, buffer);
    }
    return size;
}

static int alsa_pcm_poll(struct vfs_node *node, int events) {
    (void)node;
    if (hda_is_present()) {
        return hda_poll_handler(events);
    }
    return (events & (POLLOUT | POLLWRNORM));
}

void alsa_emu_init(void) {
    alsa_emu_register_vfs();
}

void alsa_emu_register_vfs(void) {
    // 1. Register /dev/snd/controlC0
    vfs_node_t *ctl_node = kmalloc(sizeof(vfs_node_t));
    if (ctl_node) {
        vfs_node_init(ctl_node);
        strcpy(ctl_node->name, "controlC0");
        ctl_node->flags = FS_CHARDEV;
        ctl_node->mask = 0666;
        ctl_node->length = 0;
        ctl_node->ioctl = alsa_ctl_ioctl;
        fb_register_device_node("snd/controlC0", ctl_node);
        fb_register_device_node("controlC0", ctl_node);
    }

    // 2. Register /dev/snd/pcmC0D0p
    vfs_node_t *pcm_node = kmalloc(sizeof(vfs_node_t));
    if (pcm_node) {
        vfs_node_init(pcm_node);
        strcpy(pcm_node->name, "pcmC0D0p");
        pcm_node->flags = FS_CHARDEV;
        pcm_node->mask = 0666;
        pcm_node->length = 0;
        pcm_node->write = alsa_pcm_write;
        pcm_node->ioctl = alsa_pcm_ioctl;
        pcm_node->poll = alsa_pcm_poll;
        if (hda_is_present()) {
            pcm_node->wait_queue = hda_get_wait_queue();
        }
        fb_register_device_node("snd/pcmC0D0p", pcm_node);
        fb_register_device_node("pcmC0D0p", pcm_node);
    }

    // 3. Register /dev/snd/timer
    vfs_node_t *timer_node = kmalloc(sizeof(vfs_node_t));
    if (timer_node) {
        vfs_node_init(timer_node);
        strcpy(timer_node->name, "timer");
        timer_node->flags = FS_CHARDEV;
        timer_node->mask = 0666;
        timer_node->length = 0;
        fb_register_device_node("snd/timer", timer_node);
        fb_register_device_node("timer", timer_node);
    }
}
