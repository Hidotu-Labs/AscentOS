#include "audio_dsp.h"
#include "../../fb/framebuffer.h"
#include "../../fs/vfs.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "ac97.h"
#include "sb16.h"
#include <stdbool.h>
#include <stdint.h>

// Audio device priority: AC97 (PCI) preferred over SB16 (ISA)
static bool hda_available = false;
static bool ac97_available = false;
static bool sb16_available = false;

// OSS ioctl constants (shared between drivers)
#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_STEREO 0xC0045003
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define AFMT_U8 0x00000008
#define AFMT_S16_LE 0x00000010

// Check which audio devices are available
void audio_dsp_init(void) {
  // AC97 and SB16 are already initialized by kernel.c
  // We just need to detect which ones are present by checking if they
  // registered
  vfs_node_t *hda_node = fb_lookup_device("hda_audio");
  vfs_node_t *ac97_node = fb_lookup_device("ac97");
  vfs_node_t *sb16_node = fb_lookup_device("sb16");

  hda_available = (hda_node != NULL);
  ac97_available = (ac97_node != NULL);
  sb16_available = (sb16_node != NULL);
}

// Get the active audio device's VFS node
static vfs_node_t *get_active_audio_node(void) {
  // Prefer HDA (Modern PCI) > AC97 (PCI) > SB16 (ISA)
  if (hda_available) {
    return fb_lookup_device("hda_audio");
  }
  if (ac97_available) {
    return fb_lookup_device("ac97");
  }
  if (sb16_available) {
    return fb_lookup_device("sb16");
  }
  return NULL;
}

// Dispatch write to active audio device
static uint32_t dsp_vfs_write(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
  (void)node;
  vfs_node_t *audio = get_active_audio_node();
  if (!audio || !audio->write) {
    return 0;
  }
  return audio->write(audio, offset, size, buffer);
}

// Dispatch ioctl to active audio device
static int dsp_vfs_ioctl(struct vfs_node *node, uint32_t request,
                         uint64_t arg) {
  (void)node;

  // Handle OSS version request centrally
  if (request == 0x80044D76) { // OSS_GETVERSION
    int *version = (int *)arg;
    if (!version) return -14; // EFAULT
    *version = 0x040000; // Report OSS 4.0
    return 0;
  }

  vfs_node_t *audio = get_active_audio_node();
  if (!audio || !audio->ioctl) {
    return -6; // ENXIO - no such device
  }
  return audio->ioctl(audio, request, arg);
}

// Register /dev/dsp as a dispatcher that routes to available audio hardware
void audio_dsp_register_vfs(void) {
  // First detect which devices are available
  audio_dsp_init();

  if (!hda_available && !ac97_available && !sb16_available) {
    return; // No audio hardware available
  }

  // Create the dispatcher node for /dev/dsp
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node) {
    return;
  }

  vfs_node_init(node);
  strcpy(node->name, "dsp");
  node->flags = FS_CHARDEV;
  node->mask = 0666;
  node->length = 0;
  node->write = dsp_vfs_write;
  node->ioctl = dsp_vfs_ioctl;

  // Register in internal device registry
  fb_register_device_node("dsp", node);
}
