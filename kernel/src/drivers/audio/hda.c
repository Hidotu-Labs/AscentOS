#include "hda.h"
#include "hal/hal.h"
#include "../../console/klog.h"
#include "../../cpu/irq.h"
#include "../../cpu/isr.h"
#include "../../io/io.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../sched/sched.h"
#include "../pci/pci.h"

static struct pci_device *hda_pci_dev = NULL;
static uint64_t hda_regs_phys = 0;
static void *hda_regs_virt = NULL;
static bool hda_present = false;

static uint32_t *hda_corb = NULL;
static uint64_t hda_corb_phys = 0;
static uint64_t *hda_rirb = NULL;
static uint64_t hda_rirb_phys = 0;
static volatile uint32_t rirb_read_ptr = 0;
static uint8_t hda_irq = 0;
static uint8_t hda_dac_node = 2;
static uint8_t hda_pin_node = 3;
static uint8_t hda_codec_addr = 0;

#define HDA_RING_SIZE (256 * 1024)
static uint8_t *hda_ring = NULL;
static volatile uint32_t ring_head = 0;
static volatile uint32_t ring_tail = 0;
static volatile uint32_t ring_count = 0;
static volatile bool hda_is_playing = false;

static volatile int hda_next_fill_slot = 0; // next BDL slot index to refill
// DMA pointers for VFS stream
static struct hda_bdl_entry *hda_vfs_bdl = NULL;
static uint64_t hda_vfs_bdl_phys = 0;
static uint8_t *hda_vfs_buf = NULL;
static uint64_t hda_vfs_buf_phys = 0;
#define HDA_VFS_BUF_SIZE 32768
#define HDA_VFS_NUM_BDL 4
#define HDA_VFS_TOTAL_SIZE (HDA_VFS_BUF_SIZE * HDA_VFS_NUM_BDL)

#include "../../syscalls/sys_io_shared.h"

#define SNDCTL_DSP_GETODELAY 0x80044D1D

static uint32_t hda_sample_rate = 48000;
static uint8_t hda_channels = 2;
static uint8_t hda_bits = 16;

static uint32_t active_rate = 0;
static uint8_t active_channels = 0;
static uint8_t active_bits = 0;
static volatile uint32_t hda_oss_bytes_played = 0;
static volatile uint32_t hda_oss_blocks = 0;
static volatile int hda_trigger = PCM_ENABLE_OUTPUT;

// Convert rate/channels/bits to HDA format descriptor
static uint16_t hda_format_to_hw(uint32_t rate, uint8_t channels,
                                 uint8_t bits) {
  uint16_t fmt = 0;

  // Channels (0-indexed)
  fmt |= (channels - 1) & 0x0F;

  // Bits per sample
  if (bits == 8)
    fmt |= (0 << 4);
  else if (bits == 16)
    fmt |= (1 << 4);
  else if (bits == 20)
    fmt |= (2 << 4);
  else if (bits == 24)
    fmt |= (3 << 4);
  else if (bits == 32)
    fmt |= (4 << 4);

  // Rate
  if (rate == 44100)
    fmt |= (1 << 14); // Base 44.1kHz
  else if (rate == 48000)
    fmt |= (0 << 14); // Base 48kHz
  else if (rate == 88200)
    fmt |= (1 << 14) | (1 << 11); // 44.1 * 2
  else if (rate == 96000)
    fmt |= (0 << 14) | (1 << 11); // 48 * 2
  else if (rate == 22050)
    fmt |= (1 << 14) | (1 << 8); // 44.1 / 2
  else if (rate == 11025)
    fmt |= (1 << 14) | (3 << 8); // 44.1 / 4
  else if (rate == 8000)
    fmt |= (0 << 14) | (5 << 8); // 48 / 6

  return fmt;
}

// Register Access Helpers
static inline void hda_write8(uint32_t reg, uint8_t val) {
  *(volatile uint8_t *)((uint8_t *)hda_regs_virt + reg) = val;
}

static inline void hda_write16(uint32_t reg, uint16_t val) {
  *(volatile uint16_t *)((uint8_t *)hda_regs_virt + reg) = val;
}

static inline void hda_write32(uint32_t reg, uint32_t val) {
  *(volatile uint32_t *)((uint8_t *)hda_regs_virt + reg) = val;
}

static inline uint8_t hda_read8(uint32_t reg) {
  return *(volatile uint8_t *)((uint8_t *)hda_regs_virt + reg);
}

static inline uint16_t hda_read16(uint32_t reg) {
  return *(volatile uint16_t *)((uint8_t *)hda_regs_virt + reg);
}

static inline uint32_t hda_read32(uint32_t reg) {
  return *(volatile uint32_t *)((uint8_t *)hda_regs_virt + reg);
}

// CORB / RIRB Implementation

static void hda_init_corb_rirb(void) {
  klog_puts("[HDA] Initializing CORB/RIRB...\n");

  // 1. Stop CORB/RIRB engines
  hda_write8(HDA_CORBCTL, 0);
  hda_write8(HDA_RIRBCTL, 0);

  // 2. Allocate memory (4KB is enough for both)
  // Force to 32-bit address space for maximum compatibility during discovery
  uint64_t ring_mem_phys = (uint64_t)pmm_alloc_pages_constrained(1, 0xFFFFFFFF);
  void *ring_mem_virt = (void *)(ring_mem_phys + pmm_get_hhdm_offset());
  memset(ring_mem_virt, 0, 4096);

  hda_corb_phys = ring_mem_phys;
  hda_corb = (uint32_t *)ring_mem_virt;

  hda_rirb_phys = ring_mem_phys + 2048; // Offset RIRB by 2KB
  hda_rirb = (uint64_t *)((uint8_t *)ring_mem_virt + 2048);

  // 3. Set CORB base address
  hda_write32(HDA_CORBLBASE, (uint32_t)hda_corb_phys);
  hda_write32(HDA_CORBUBASE, (uint32_t)(hda_corb_phys >> 32));

  // Reset CORB read pointer, set write pointer to 0
  hda_write16(HDA_CORBRP, (1 << 15)); // Reset bit
  // Wait for it to reset (hardware clears the bit)
  for (int i = 0; i < 10000; i++) {
    if (!(hda_read16(HDA_CORBRP) & (1 << 15)))
      break;
    io_wait();
  }
  hda_write16(HDA_CORBRP, 0);
  hda_write16(HDA_CORBWP, 0);

  // Set CORB size (0=2 entries, 1=16, 2=256)
  hda_write8(HDA_CORBSIZE, 0x02);

  // 4. Set RIRB base address
  hda_write32(HDA_RIRBLBASE, (uint32_t)hda_rirb_phys);
  hda_write32(HDA_RIRBUBASE, (uint32_t)(hda_rirb_phys >> 32));

  // Reset RIRB write pointer
  hda_write16(HDA_RIRBWP, (1 << 15)); // Reset bit
  for (int i = 0; i < 10000; i++) {
    if (!(hda_read16(HDA_RIRBWP) & (1 << 15)))
      break;
    io_wait();
  }

  hda_write16(HDA_RINTCNT, 1); // Interrupt after 1 response

  // Set RIRB size (0=2, 1=16, 2=256)
  hda_write8(HDA_RIRBSIZE, 0x02);

  rirb_read_ptr = hda_read16(HDA_RIRBWP) & 0xFF;

  // 5. Start CORB/RIRB engines
  hda_write8(HDA_CORBCTL, HDA_CORBCTL_RUN);
  hda_write8(HDA_RIRBCTL, HDA_RIRBCTL_RUN);
}

uint32_t hda_send_verb(uint8_t codec, uint8_t node, uint32_t verb,
                       uint16_t payload) {
  if (!hda_present || !hda_corb)
    return 0xFFFFFFFF;

  // Verb format: [31:28] Codec, [27:20] Node, [19:0] Verb + Payload
  uint32_t command =
      ((uint32_t)codec << 28) | ((uint32_t)node << 20) | (verb << 8) | payload;

  // HDA Spec 1.0a: CORBWP is the index of the last valid command
  uint16_t wp = hda_read16(HDA_CORBWP);
  uint16_t next_wp = (wp + 1) % HDA_CORB_ENTRIES;

  // Wait for space in CORB (should be immediate since we are single threaded
  // here) hda_read16(HDA_CORBRP) is the HW read pointer

  // Write command into CORB slot and flush to RAM
  hda_corb[next_wp] = command;
  __asm__ volatile("clflush (%0)" : : "r"(&hda_corb[next_wp]) : "memory");
  hda_write16(HDA_CORBWP, next_wp);

  // Wait for response in RIRB
  int timeout = 100000;
  while (timeout--) {
    uint16_t hw_rirbwp = hda_read16(HDA_RIRBWP) & 0xFF;
    if (hw_rirbwp != rirb_read_ptr) {
      rirb_read_ptr = (rirb_read_ptr + 1) % HDA_RIRB_ENTRIES;
      // Flush before read
      __asm__ volatile("clflush (%0)"
                       :
                       : "r"(&hda_rirb[rirb_read_ptr])
                       : "memory");
      uint64_t resp_entry = hda_rirb[rirb_read_ptr];
      // HDA Spec: Response is in the lower 32 bits. Upper 32 contains
      // flags/codec bits.
      return (uint32_t)(resp_entry & 0xFFFFFFFF);
    }
    io_wait();
  }

  klog_puts("[HDA] Verb Response Timeout (CORB/RIRB)!\n");
  return 0xFFFFFFFF;
}

uint32_t hda_send_verb_immediate(uint8_t codec, uint8_t node, uint32_t verb,
                                 uint16_t payload) {
  if (!hda_present)
    return 0xFFFFFFFF;

  // Wait for Immediate Command engine to be ready
  int timeout = 10000;
  while ((hda_read16(HDA_ICS) & 0x01) && timeout--)
    io_wait();

  // Command format: [31:28] Codec, [27:20] Node, [19:0] Verb + Payload
  uint32_t command =
      ((uint32_t)codec << 28) | ((uint32_t)node << 20) | (verb << 8) | payload;

  hda_write32(HDA_ICO, command);
  // Set ICB bit to start execution
  hda_write16(HDA_ICS, hda_read16(HDA_ICS) | 0x01);

  timeout = 10000;
  while (timeout--) {
    uint16_t ics = hda_read16(HDA_ICS);
    if (!(ics & 0x01) && (ics & 0x02)) { // ICB=0 (done), IRV=1 (valid response)
      return hda_read32(HDA_IRI);
    }
    io_wait();
  }

  klog_puts("[HDA] Verb Response Timeout (Immediate)!\n");
  return 0xFFFFFFFF;
}

static void hda_stop_stream_locked(void);
static bool hda_start_stream_locked(void);

static void hda_flush_cache(void *ptr, size_t size) {
  uint8_t *p = (uint8_t *)ptr;
  for (size_t i = 0; i < size; i += 64) {
    __asm__ volatile("clflush (%0)" : : "r"(p + i) : "memory");
  }
}

// Fill a single BDL buffer slot with audio data from the ring buffer
static void hda_fill_buffer(int buf_idx) {
  if (!hda_vfs_buf || buf_idx < 0 || buf_idx >= HDA_VFS_NUM_BDL)
    return;

  uint8_t *dest = hda_vfs_buf + (buf_idx * HDA_VFS_BUF_SIZE);

  if (ring_count == 0) {
    memset(dest, 0, HDA_VFS_BUF_SIZE);
    hda_flush_cache(dest, HDA_VFS_BUF_SIZE);
    return;
  }

  uint32_t chunk =
      (ring_count > HDA_VFS_BUF_SIZE) ? HDA_VFS_BUF_SIZE : ring_count;

  // Read from ring
  uint32_t unread1 = HDA_RING_SIZE - ring_tail;
  if (chunk <= unread1) {
    memcpy(dest, hda_ring + ring_tail, chunk);
    ring_tail = (ring_tail + chunk) % HDA_RING_SIZE;
  } else {
    memcpy(dest, hda_ring + ring_tail, unread1);
    uint32_t rem = chunk - unread1;
    memcpy(dest + unread1, hda_ring, rem);
    ring_tail = rem;
  }

  if (chunk < HDA_VFS_BUF_SIZE) {
    memset(dest + chunk, 0, HDA_VFS_BUF_SIZE - chunk);
  }
  hal_irq_disable();
  ring_count -= chunk;
  hal_irq_enable();
  hda_oss_bytes_played += HDA_VFS_BUF_SIZE;
  hda_oss_blocks++;

  // Flush cache for the buffer
  hda_flush_cache(dest, HDA_VFS_BUF_SIZE);
}

static uint32_t hda_output_sd_off(void) {
  uint16_t gcap = hda_read16(HDA_GCAP);
  int iss = (gcap >> 8) & 0xF;
  return HDA_SD_BASE + (iss * 0x20);
}

static void hda_pump_audio(void) {
  if (!hda_vfs_buf)
    return;

  if (!hda_is_playing) {
    if (ring_count >= HDA_VFS_BUF_SIZE)
      hda_start_stream_locked();
    return;
  }

  uint32_t sd_off = hda_output_sd_off();
  uint32_t lpib   = hda_read32(sd_off + HDA_SD_LPIB);
  int hw_buf      = (int)(lpib / HDA_VFS_BUF_SIZE) % HDA_VFS_NUM_BDL;

  // Refill all slots between hda_next_fill_slot and the current hardware DMA position
  while (hda_next_fill_slot != hw_buf) {
    int slot = hda_next_fill_slot;
    hda_fill_buffer(slot);
    hda_next_fill_slot = (slot + 1) % HDA_VFS_NUM_BDL;
  }
}

static void hda_stop_stream_locked(void) {
  if (!hda_is_playing)
    return;
  uint32_t sd_off = hda_output_sd_off();
  hda_write32(sd_off + HDA_SD_CTL,
              hda_read32(sd_off + HDA_SD_CTL) & ~HDA_SD_CTL_RUN);
  hda_is_playing = false;
}

static int hda_fill_optr(struct oss_count_info *info) {
  if (!info)
    return -14;
  info->bytes = (int)hda_oss_bytes_played;
  info->blocks = (int)hda_oss_blocks;
  if (hda_is_playing)
    info->ptr = (int)hda_read32(hda_output_sd_off() + HDA_SD_LPIB);
  else
    info->ptr = (int)(hda_oss_bytes_played % HDA_VFS_TOTAL_SIZE);
  return 0;
}

// Start the output stream. Must be called with interrupts disabled.
// Returns true if a new stream was started.
static bool hda_start_stream_locked(void) {
  if (hda_is_playing)
    return false;

  if (ring_count < HDA_VFS_BUF_SIZE)
    return false;

  active_rate = hda_sample_rate;
  active_channels = hda_channels;
  active_bits = hda_bits;
  hda_is_playing = true;

  uint16_t gcap = hda_read16(HDA_GCAP);
  int iss = (gcap >> 8) & 0xF;
  uint32_t sd_off = HDA_SD_BASE + (iss * 0x20);

  // Stop and reset stream
  hda_write32(sd_off + HDA_SD_CTL, 0);
  hda_write32(sd_off + HDA_SD_CTL, HDA_SD_CTL_SRST);
  for (int i = 0; i < 1000; i++)
    if (hda_read32(sd_off + HDA_SD_CTL) & HDA_SD_CTL_SRST)
      break;
  hda_write32(sd_off + HDA_SD_CTL, 0);
  for (int i = 0; i < 1000; i++)
    if (!(hda_read32(sd_off + HDA_SD_CTL) & HDA_SD_CTL_SRST))
      break;

  // Set BDL address for the VFS buffers
  hda_write32(sd_off + HDA_SD_BDPL, (uint32_t)hda_vfs_bdl_phys);
  hda_write32(sd_off + HDA_SD_BDPU, (uint32_t)(hda_vfs_bdl_phys >> 32));

  hda_write32(sd_off + HDA_SD_CBL, HDA_VFS_TOTAL_SIZE);
  hda_write16(sd_off + HDA_SD_LVI, HDA_VFS_NUM_BDL - 1);

  // Flush BDL to memory
  hda_flush_cache(hda_vfs_bdl, sizeof(struct hda_bdl_entry) * HDA_VFS_NUM_BDL);

  // Pre-fill DMA buffers. Fill only as many slots as the ring has data for;
  // leave the rest as silence (they were zeroed at allocation time). The DMA
  // engine takes ~(HDA_VFS_BUF_SIZE / bytes_per_sec) seconds per slot, so by
  // the time it reaches an unfilled slot the IRQ handler will have refilled it.
  hda_next_fill_slot = 0;
  for (int i = 0; i < HDA_VFS_NUM_BDL; i++) {
    if (ring_count >= HDA_VFS_BUF_SIZE)
      hda_fill_buffer(i);
    // else: slot stays as silence, pump_audio will fill it on the next IRQ
  }
  // After pre-fill, the next slot the IRQ should refill is slot 0 again
  // (since DMA starts at 0 and the first IOC fires after slot 0 completes).
  hda_next_fill_slot = 0;

  uint16_t hw_fmt = hda_format_to_hw(hda_sample_rate, hda_channels, 16);
  hda_write16(sd_off + HDA_SD_FMT, hw_fmt);

  // Update codec widgets
  // Re-enable IRQs for verb round-trips: each hda_send_verb_immediate can
  // busy-wait up to 10000 iterations.  With 5 verbs and IRQs disabled the
  // whole time, we can block audio IOCs for ~50ms — long enough to drain the
  // pre-filled DMA buffers and cause a cutoff.  The DMA engine is not yet
  // running (RUN bit not set), so re-enabling here is safe.
  hal_irq_enable();
  hda_send_verb_immediate(hda_codec_addr, hda_dac_node, HDA_VERB_SET_FORMAT,
                          hw_fmt);
  hda_send_verb_immediate(hda_codec_addr, hda_dac_node, HDA_VERB_SET_STREAM_ID,
                          (1 << 4));
  hda_send_verb_immediate(hda_codec_addr, hda_dac_node, HDA_VERB_SET_AMP_MUTE,
                          0xB000 | 0x7F);

  hda_send_verb_immediate(hda_codec_addr, hda_pin_node, HDA_VERB_SET_PIN_CTL,
                          0xC0);
  hda_send_verb_immediate(hda_codec_addr, hda_pin_node, HDA_VERB_SET_AMP_MUTE,
                          0xB000 | 0x7F);
  hal_irq_disable();

  // Enable interrupt for this stream in INTCTL
  hda_write32(HDA_INTCTL, hda_read32(HDA_INTCTL) | (1 << iss));

  // Start stream with Stream ID 1 and IOCE
  hda_write32(sd_off + HDA_SD_CTL, (1 << 20) | HDA_SD_CTL_IOCE | HDA_SD_CTL_RUN);
  return true;
}

static void hda_irq_handler(struct registers *regs) {
  (void)regs;

  uint32_t intsts = hda_read32(HDA_INTSTS);
  if (!(intsts & (1u << 31)))
    return;

  uint8_t rirbsts = hda_read8(HDA_RIRBSTS);
  if (rirbsts & 0x01) {
    hda_write8(HDA_RIRBSTS, 0x01);
  }

  uint16_t gcap = hda_read16(HDA_GCAP);
  int iss = (gcap >> 8) & 0xF;

  // Check Output Stream 0 (ISS + 0)
  if (intsts & (1 << iss)) {
    uint32_t sd_off = HDA_SD_BASE + (iss * 0x20);
    uint8_t sd_sts = hda_read8(sd_off + HDA_SD_STS);
    // Clear all interrupt status bits (IOC=0x04, FIFOE=0x08, DESE=0x10)
    if (sd_sts & 0x1C) {
      hda_write8(sd_off + HDA_SD_STS, sd_sts & 0x1C);
      if (sd_sts & 0x04) {
        hda_pump_audio();
      }
    }
  }
}

void hda_init(void) {
  klog_puts("[HDA] Searching for Intel HDA controller...\n");

  // HDA is Class 0x04 (Multimedia), Subclass 0x03 (High Definition Audio)
  hda_pci_dev = pci_find_device(0x04, 0x03);

  if (!hda_pci_dev) {
    klog_puts("[HDA] No controller found.\n");
    return;
  }

  klog_puts("[HDA] Found controller at ");
  klog_uint64(hda_pci_dev->bus);
  klog_putchar(':');
  klog_uint64(hda_pci_dev->slot);
  klog_putchar('.');
  klog_uint64(hda_pci_dev->func);
  klog_puts(" (Vendor: 0x");
  klog_uint64(hda_pci_dev->vendor_id);
  klog_puts(", Device: 0x");
  klog_uint64(hda_pci_dev->device_id);
  klog_puts(")\n");
  klog_puts(
      "[HDA] Intel High Definition Audio Controller found and enabled.\n");

  pci_enable_bus_mastering(hda_pci_dev);

  // HDA uses BAR0 for memory mapped registers.
  // Check if it's a 64-bit BAR
  uint32_t bar0 = hda_pci_dev->bar[0];
  hda_regs_phys = bar0 & ~0xF;

  if ((bar0 & 0x6) == 0x4) { // 64-bit BAR
    hda_regs_phys |= ((uint64_t)hda_pci_dev->bar[1]) << 32;
  }
  hda_irq = hda_pci_dev->irq_line;

  klog_puts("[HDA] BAR0 physical address: 0x");
  klog_uint64(hda_regs_phys);
  klog_puts("\n");

  // Map to virtual space using HHDM
  hda_regs_virt = (void *)(hda_regs_phys + pmm_get_hhdm_offset());
  hda_present = true;

  // Clear STATESTS (Write 1 to Clear)
  hda_write16(HDA_STATESTS, hda_read16(HDA_STATESTS));

  // Install Interrupt Handler
  if (hda_irq > 0) {
    klog_puts("[HDA] Installing IRQ ");
    klog_uint64(hda_irq);
    klog_puts("\n");
    irq_install_handler(hda_irq, hda_irq_handler, 0x000F);
  }

  // 1. Controller Reset (Required before CORB/RIRB init)
  klog_puts("[HDA] Performing Controller Reset...\n");
  uint32_t gctl = hda_read32(HDA_GCTL);
  hda_write32(HDA_GCTL, gctl & ~HDA_GCTL_CRST);

  int timeout = 10000;
  while ((hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout--) {
    io_wait();
  }

  if (hda_read32(HDA_GCTL) & HDA_GCTL_CRST) {
    klog_puts("[ERR] HDA failed to enter reset state!\n");
    return;
  }

  hda_write32(HDA_GCTL, hda_read32(HDA_GCTL) | HDA_GCTL_CRST);

  timeout = 10000;
  while (!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout--) {
    io_wait();
  }

  if (!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST)) {
    klog_puts("[ERR] HDA failed to exit reset state!\n");
    return;
  }

  // Wait for codecs to enumerate
  for (int i = 0; i < 10000; i++)
    io_wait();

  // 2. CORB / RIRB Initialization
  hda_init_corb_rirb();

  // 3. Widget Discovery
  uint16_t statests = hda_read16(HDA_STATESTS);
  for (int c = 0; c < 15; c++) {
    if (statests & (1 << c)) {
      hda_codec_addr = c;
      uint32_t nodes = hda_send_verb_immediate(c, 0, HDA_VERB_GET_PARAM,
                                               HDA_PARAM_NODE_COUNT);
      uint8_t start = (nodes >> 16) & 0xFF;
      uint8_t total = nodes & 0xFF;
      for (uint8_t fg = start; fg < start + total; fg++) {
        uint32_t w_nodes = hda_send_verb_immediate(c, fg, HDA_VERB_GET_PARAM,
                                                   HDA_PARAM_NODE_COUNT);
        uint8_t w_start = (w_nodes >> 16) & 0xFF;
        uint8_t w_total = w_nodes & 0xFF;
        for (uint8_t w = w_start; w < w_start + w_total; w++) {
          uint32_t cap =
              hda_send_verb_immediate(c, w, HDA_VERB_GET_PARAM, HDA_PARAM_CAPS);
          uint8_t type = (cap >> 20) & 0xF;
          if (type == HDA_WIDGET_AUDIO_OUT)
            hda_dac_node = w;
          if (type == HDA_WIDGET_PIN_COMPLEX)
            hda_pin_node = w;
        }
      }
      break;
    }
  }
  klog_puts("[HDA] Discovered DAC: ");
  klog_uint64(hda_dac_node);
  klog_puts(", Pin: ");
  klog_uint64(hda_pin_node);
  klog_puts("\n");

  // 4. Power up and Unmute everything (Brute force to ensure routing)
  uint32_t nodes = hda_send_verb_immediate(
      hda_codec_addr, 0, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT);
  uint8_t start = (nodes >> 16) & 0xFF;
  uint8_t total = nodes & 0xFF;
  for (uint8_t fg = start; fg < start + total; fg++) {
    hda_send_verb_immediate(hda_codec_addr, fg, 0x705, 0x0); // D0
    uint32_t w_nodes = hda_send_verb_immediate(
        hda_codec_addr, fg, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT);
    uint8_t w_start = (w_nodes >> 16) & 0xFF;
    uint8_t w_total = w_nodes & 0xFF;
    for (uint8_t w = w_start; w < w_start + w_total; w++) {
      hda_send_verb_immediate(hda_codec_addr, w, 0x705, 0x0); // D0
      // Unmute and set max gain for all possible amplifier combinations
      hda_send_verb_immediate(hda_codec_addr, w, 0x300,
                              0xB000 |
                                  0x7F); // Output Amp, Left+Right, Gain 127
      hda_send_verb_immediate(hda_codec_addr, w, 0x300,
                              0x7000 | 0x7F); // Input Amp, Left+Right, Gain 127

      uint32_t cap = hda_send_verb_immediate(
          hda_codec_addr, w, HDA_VERB_GET_PARAM, HDA_PARAM_CAPS);
      uint8_t type = (cap >> 20) & 0xF;
      if (type == HDA_WIDGET_PIN_COMPLEX) {
        hda_send_verb_immediate(hda_codec_addr, w, 0x707,
                                0xC0); // Output Enable + HP Amp Enable
        hda_send_verb_immediate(hda_codec_addr, w, 0x70C, 0x2); // EAPD Enable
      }
    }
  }

  // 4. Enable Global Interrupts
  hda_write32(HDA_INTCTL, (1u << 31)); // GIE

  // 5. Register with VFS
  hda_register_vfs();
}

void hda_phase1_test(void) {
  if (!hda_present)
    return;

  klog_puts("[HDA] Running Phase 1 Test...\n");

  // Read Version
  uint8_t maj = hda_read8(HDA_VMAJ);
  uint8_t min = hda_read8(HDA_VMIN);
  klog_puts("[HDA] Specification Version: ");
  klog_uint64(maj);
  klog_putchar('.');
  klog_uint64(min);
  klog_puts("\n");

  // Read Capabilities
  uint16_t gcap = hda_read16(HDA_GCAP);
  klog_puts("[HDA] Global Capabilities (GCAP): 0x");
  klog_uint64(gcap);
  klog_puts("\n");
  klog_puts("      - ISS (Input Streams): ");
  klog_uint64((gcap >> 8) & 0xF);
  klog_puts("\n      - OSS (Output Streams): ");
  klog_uint64((gcap >> 12) & 0xF);
  klog_puts("\n      - BSS (Bidirectional): ");
  klog_uint64((gcap >> 3) & 0x1F);
  klog_puts("\n      - 64-bit addr: ");
  klog_puts((gcap & 1) ? "Yes" : "No");
  klog_puts("\n");

  // Controller Reset Test
  klog_puts("[HDA] Performing Controller Reset...\n");

  // 1. Clear CRST bit to enter reset
  uint32_t gctl = hda_read32(HDA_GCTL);
  hda_write32(HDA_GCTL, gctl & ~HDA_GCTL_CRST);

  // 2. Wait for CRST to become 0
  int timeout = 10000;
  while ((hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout--) {
    io_wait();
  }

  if (hda_read32(HDA_GCTL) & HDA_GCTL_CRST) {
    klog_puts("[ERR] HDA failed to enter reset state!\n");
    return;
  }

  // 3. Set CRST bit to exit reset
  hda_write32(HDA_GCTL, hda_read32(HDA_GCTL) | HDA_GCTL_CRST);

  // 4. Wait for CRST to become 1
  timeout = 10000;
  while (!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST) && timeout--) {
    io_wait();
  }

  if (!(hda_read32(HDA_GCTL) & HDA_GCTL_CRST)) {
    klog_puts("[ERR] HDA failed to exit reset state!\n");
    return;
  }

  // Crucial: Wait for codecs to enumerate after CRST=1
  for (int i = 0; i < 10000; i++)
    io_wait();

  klog_puts("[OK] HDA Controller Reset Successful. Phase 1 Test PASSED.\n");
}

void hda_phase2_test(void) {
  if (!hda_present)
    return;

  klog_puts("[HDA] Running Phase 2 Test (Codec Discovery)...\n");

  // Scan STATESTS to see which codecs are alive
  uint16_t statests = hda_read16(HDA_STATESTS);
  klog_puts("[HDA] STATESTS: 0x");
  klog_uint64(statests);
  klog_puts("\n");

  for (int i = 0; i < 15; i++) {
    if (statests & (1 << i)) {
      klog_puts("[HDA] Found Codec at index ");
      klog_uint64(i);

      // Try Immediate Command first (it's often more reliable for early
      // detection)
      uint32_t vid = hda_send_verb_immediate(i, 0, 0xF00, 0x00);
      if (vid == 0xFFFFFFFF) {
        klog_puts(". Immediate Fail, trying CORB...");
        vid = hda_send_verb(i, 0, 0xF00, 0x00);
      }

      klog_puts(". Vendor ID: 0x");
      klog_uint64(vid);
      klog_puts("\n");
    }
  }

  klog_puts("[OK] HDA Phase 2 Test Complete.\n");
}

void hda_phase3_test(void) {
  if (!hda_present)
    return;

  klog_puts("[HDA] Running Phase 3 (Widget Enumeration)...\n");

  uint16_t statests = hda_read16(HDA_STATESTS);
  for (int i = 0; i < 15; i++) {
    if (!(statests & (1 << i)))
      continue;

    klog_puts("[HDA] Enumerating Codec ");
    klog_uint64(i);
    klog_puts("\n");

    // Root node (0) always lists the first level of nodes (Function Groups)
    uint32_t nodes =
        hda_send_verb_immediate(i, 0, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT);
    if (nodes == 0xFFFFFFFF)
      continue;

    uint8_t start_node = (nodes >> 16) & 0xFF;
    uint8_t total_nodes = nodes & 0xFF;

    klog_puts("      - Function Groups: Start=");
    klog_uint64(start_node);
    klog_puts(", Total=");
    klog_uint64(total_nodes);
    klog_puts("\n");

    for (uint8_t fg = start_node; fg < start_node + total_nodes; fg++) {
      uint32_t fg_type =
          hda_send_verb_immediate(i, fg, HDA_VERB_GET_PARAM, HDA_PARAM_FG_TYPE);
      klog_puts("      - FG ");
      klog_uint64(fg);
      klog_puts(": Type 0x");
      klog_uint64(fg_type & 0xFF);

      if ((fg_type & 0xFF) == 1) { // Audio Function Group
        klog_puts(" (Audio AFG)\n");

        // Read widget range within this AFG
        uint32_t w_nodes = hda_send_verb_immediate(i, fg, HDA_VERB_GET_PARAM,
                                                   HDA_PARAM_NODE_COUNT);
        uint8_t w_start = (w_nodes >> 16) & 0xFF;
        uint8_t w_total = w_nodes & 0xFF;

        klog_puts("        - Widgets: Start=");
        klog_uint64(w_start);
        klog_puts(", Total=");
        klog_uint64(w_total);
        klog_puts("\n");

        for (uint8_t w = w_start; w < w_start + w_total; w++) {
          uint32_t cap =
              hda_send_verb_immediate(i, w, HDA_VERB_GET_PARAM, HDA_PARAM_CAPS);
          uint8_t type = (cap >> 20) & 0xF;

          if (type == HDA_WIDGET_AUDIO_OUT)
            klog_puts("        - Node ");
          else if (type == HDA_WIDGET_PIN_COMPLEX)
            klog_puts("        - Node ");
          else if (type == HDA_WIDGET_BEEP_GEN)
            klog_puts("        - Node ");
          else
            continue; // Skip others for brevity in logs

          klog_uint64(w);
          klog_puts(": Type ");
          klog_uint64(type);
          if (type == HDA_WIDGET_BEEP_GEN)
            klog_puts(" [BEEP GENERATOR]");
          klog_puts("\n");
        }
      } else {
        klog_puts("\n");
      }
    }
  }
}

void hda_beep_test(void) {
  if (!hda_present)
    return;

  klog_puts("[HDA] Searching for BEEP widget...\n");

  uint16_t statests = hda_read16(HDA_STATESTS);
  for (int c = 0; c < 15; c++) {
    if (!(statests & (1 << c)))
      continue;

    uint32_t nodes =
        hda_send_verb_immediate(c, 0, HDA_VERB_GET_PARAM, HDA_PARAM_NODE_COUNT);
    uint8_t start_node = (nodes >> 16) & 0xFF;
    uint8_t total_nodes = nodes & 0xFF;

    for (uint8_t fg = start_node; fg < start_node + total_nodes; fg++) {
      uint32_t w_nodes = hda_send_verb_immediate(c, fg, HDA_VERB_GET_PARAM,
                                                 HDA_PARAM_NODE_COUNT);
      uint8_t w_start = (w_nodes >> 16) & 0xFF;
      uint8_t w_total = w_nodes & 0xFF;

      for (uint8_t w = w_start; w < w_start + w_total; w++) {
        uint32_t cap =
            hda_send_verb_immediate(c, w, HDA_VERB_GET_PARAM, HDA_PARAM_CAPS);
        if (((cap >> 20) & 0xF) == HDA_WIDGET_BEEP_GEN) {
          klog_puts("[HDA] Found BEEP node ");
          klog_uint64(w);
          klog_puts(" on Codec ");
          klog_uint64(c);
          klog_puts(". Beeping for 1 second...\n");

          // Unmute widget (Amplifier Gain/Mute)
          // HDA_VERB_SET_AMP_MUTE (0x3): [15] Output, [14] Input, [13] Left,
          // [12] Right, [11:8] Index, [7] Mute, [6:0] Gain
          hda_send_verb_immediate(c, w, HDA_VERB_SET_AMP_MUTE,
                                  0xB000 | 0x7F); // Unmute + max gain

          // Start beep (payload 0x40 ~ 200Hz-ish)
          hda_send_verb_immediate(c, w, HDA_VERB_SET_BEEP, 0x40);

          // Simple delay
          for (int i = 0; i < 500000; i++)
            io_wait();

          // Stop beep
          hda_send_verb_immediate(c, w, HDA_VERB_SET_BEEP, 0x00);
          klog_puts("[HDA] Beep complete.\n");
          return;
        }
      }
    }
  }
  klog_puts("[HDA] No beep widget found.\n");
}

void hda_phase4_test(void) {
  if (!hda_present)
    return;

  klog_puts("[HDA] Running Phase 4 (DMA Audio Test)...\n");

  // 1. Allocate DMA memory
  // BDL: 2 entries * 16 bytes = 32 bytes (aligned to 128)
  // Buffers: 2 * 4096 = 8192 bytes
  uint64_t dma_phys =
      (uint64_t)pmm_alloc_pages_constrained(3, 0xFFFFFFFF); // 3 pages (12KB)
  void *dma_virt = (void *)(dma_phys + pmm_get_hhdm_offset());
  memset(dma_virt, 0, 12288);

  struct hda_bdl_entry *bdl = (struct hda_bdl_entry *)dma_virt;
  uint8_t *audio_buf = (uint8_t *)dma_virt + 4096;

  // Fill audio buffer with a square wave (440Hz A4)
  // 48000 samples/sec, 440 cycles/sec -> ~109 samples per cycle
  // Stereo 16-bit: [L_low, L_high, R_low, R_high] = 4 bytes per sample
  int16_t *samples = (int16_t *)audio_buf;
  for (int i = 0; i < 4096 / 2; i++) { // 2048 stereo samples
    int cycle_pos = i % 109;
    int16_t val = (cycle_pos < 54) ? 4000 : -4000;
    samples[i * 2] = val;     // Left
    samples[i * 2 + 1] = val; // Right
  }

  // 2. Setup BDL
  bdl[0].addr_low = (uint32_t)(dma_phys + 4096);
  bdl[0].addr_high = (uint32_t)((dma_phys + 4096) >> 32);
  bdl[0].length = 4096;
  bdl[0].flags = 1; // IOC

  bdl[1].addr_low = (uint32_t)(dma_phys + 8192);
  bdl[1].addr_high = (uint32_t)((dma_phys + 8192) >> 32);
  bdl[1].length = 4096;
  bdl[1].flags = 1;

  // 3. Setup Stream Descriptor 0 (First Output Stream)
  // OSS usually start after ISS. ISS=4, so OSS0 is SD4.
  uint16_t gcap = hda_read16(HDA_GCAP);
  int iss = (gcap >> 8) & 0xF;
  uint32_t sd_off = HDA_SD_BASE + (iss * 0x20);

  // Enable interrupt for this stream in INTCTL
  hda_write32(HDA_INTCTL, hda_read32(HDA_INTCTL) | (1 << iss));

  // Stop and reset stream
  hda_write32(sd_off + HDA_SD_CTL, 0);
  hda_write32(sd_off + HDA_SD_CTL, HDA_SD_CTL_SRST);
  for (int i = 0; i < 1000; i++)
    if (hda_read32(sd_off + HDA_SD_CTL) & HDA_SD_CTL_SRST)
      break;
  hda_write32(sd_off + HDA_SD_CTL, 0);
  for (int i = 0; i < 1000; i++)
    if (!(hda_read32(sd_off + HDA_SD_CTL) & HDA_SD_CTL_SRST))
      break;

  // Set BDL address
  hda_write32(sd_off + HDA_SD_BDPL, (uint32_t)dma_phys);
  hda_write32(sd_off + HDA_SD_BDPU, (uint32_t)(dma_phys >> 32));

  hda_write32(sd_off + HDA_SD_CBL, 8192);
  hda_write16(sd_off + HDA_SD_LVI, 1);

  // Format: 48kHz, 16-bit, 2 channels (0x0011)
  uint16_t fmt = 0x0011;
  hda_write16(sd_off + HDA_SD_FMT, fmt);

  // Set Stream ID to 1 (Tag 1) and Enable Interrupt on Completion (IOCE)
  hda_write32(sd_off + HDA_SD_CTL, (1 << 20) | HDA_SD_CTL_IOCE);

  // 4. Configure Codec Widgets (Hardcoded for VirtIO HDA nodes 2 and 3)
  // Set format for Node 2 (Audio Out)
  hda_send_verb_immediate(0, 2, HDA_VERB_SET_FORMAT, fmt);
  // Set Stream ID 1 on Node 2
  hda_send_verb_immediate(0, 2, HDA_VERB_SET_STREAM_ID, 0x10);

  // Unmute Node 2
  hda_send_verb_immediate(0, 2, HDA_VERB_SET_AMP_MUTE, 0xB000 | 0x7F);

  // Enable Output on Node 3 (Pin)
  hda_send_verb_immediate(0, 3, HDA_VERB_SET_PIN_CTL, 0x40);
  // Unmute Node 3
  hda_send_verb_immediate(0, 3, HDA_VERB_SET_AMP_MUTE, 0xB000 | 0x7F);

  klog_puts("[HDA] Starting audio playback...\n");
  hda_write32(sd_off + HDA_SD_CTL,
              hda_read32(sd_off + HDA_SD_CTL) | HDA_SD_CTL_RUN);

  // Play for 3 seconds
  for (int i = 0; i < 3000000; i++)
    io_wait();
  hda_write32(sd_off + HDA_SD_CTL,
              hda_read32(sd_off + HDA_SD_CTL) & ~HDA_SD_CTL_RUN);
  klog_puts("[HDA] Playback stopped. Phase 4 Test complete.\n");
}

static uint32_t hda_vfs_write(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (!hda_present || !buffer || size == 0)
    return 0;

  uint32_t written = 0;
  while (written < size) {
    hal_irq_disable();
    uint32_t cur_count = ring_count;
    hal_irq_enable();

    uint32_t space = HDA_RING_SIZE - cur_count;
    if (space == 0) {
      sched_yield(); // ring full: let the IRQ handler drain it
      continue;
    }

    uint32_t remaining = size - written;
    uint32_t chunk = (remaining > space) ? space : remaining;

    // For 8-bit audio, each source byte expands to 2 bytes (16-bit)
    if (hda_bits == 8) {
      if (chunk * 2 > space)
        chunk = space / 2;
      if (chunk == 0) {
        sched_yield();
        continue;
      }
    }

    // Copy data to ring buffer (IRQ handler reads ring_tail, writer uses ring_head)
    if (hda_bits == 8) {
      for (uint32_t i = 0; i < chunk; i++) {
        int16_t s16 = ((int16_t)(buffer[written + i] ^ 0x80)) << 8;
        hda_ring[ring_head] = (uint8_t)(s16 & 0xFF);
        ring_head = (ring_head + 1) % HDA_RING_SIZE;
        hda_ring[ring_head] = (uint8_t)((s16 >> 8) & 0xFF);
        ring_head = (ring_head + 1) % HDA_RING_SIZE;
      }
      hal_irq_disable();
      ring_count += (chunk * 2);
    } else {
      uint32_t head_snap = ring_head;
      uint32_t end1 = HDA_RING_SIZE - head_snap;
      if (chunk <= end1) {
        memcpy(hda_ring + head_snap, buffer + written, chunk);
        ring_head = (head_snap + chunk) % HDA_RING_SIZE;
      } else {
        memcpy(hda_ring + head_snap, buffer + written, end1);
        uint32_t rem = chunk - end1;
        memcpy(hda_ring, buffer + written + end1, rem);
        ring_head = rem;
      }
      hal_irq_disable();
      ring_count += chunk;
    }

    // Start once enough data is buffered to fill the first DMA slot.
    // hda_start_stream_locked() enforces the HDA_VFS_BUF_SIZE threshold.
    if (!hda_is_playing)
      hda_start_stream_locked();

    hal_irq_enable();

    written += chunk;
  }

  return written;
}

static int hda_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  (void)node;
  if (!hda_present)
    return -5;

  switch (request) {
  case SNDCTL_DSP_RESET:
  {
    hal_irq_disable();
    if (!hda_is_playing) {
      ring_head = ring_tail = ring_count = 0;
      hda_next_fill_slot = 0;
      hda_oss_bytes_played = 0;
      hda_oss_blocks = 0;
    }
    hda_trigger = PCM_ENABLE_OUTPUT;
    hal_irq_enable();
    return 0;
  }
  case SNDCTL_DSP_SYNC:
    return 0;
  case SNDCTL_DSP_GETODELAY:
  {
    int *delay = (int *)arg;
    if (!delay)
      return -14;
    hal_irq_disable();
    *delay = (int)ring_count;
    hal_irq_enable();
    return 0;
  }
  case SNDCTL_DSP_SETFRAGMENT:
    return 0;
  case SNDCTL_DSP_GETOSPACE:
  {
    struct {
      int fragments;
      int fragstotal;
      int fragsize;
      int bytes;
    } *info = (void *)arg;
    if (!info)
      return -14;
    hal_irq_disable();
    info->fragsize    = HDA_VFS_BUF_SIZE;
    info->fragstotal  = HDA_RING_SIZE / HDA_VFS_BUF_SIZE;
    info->bytes       = (int)(HDA_RING_SIZE - ring_count);
    info->fragments   = (int)(info->bytes / info->fragsize);
    hal_irq_enable();
    return 0;
  }
  case SNDCTL_DSP_GETOPTR:
  case SNDCTL_DSP_GETIPTR:
  {
    struct oss_count_info *info = (struct oss_count_info *)arg;
    hal_irq_disable();
    int rc = hda_fill_optr(info);
    hal_irq_enable();
    return rc;
  }
  case SNDCTL_DSP_GETFMTS:
  {
    int *fmts = (int *)arg;
    if (!fmts)
      return -14;
    *fmts = AFMT_S16_LE | AFMT_U8;
    return 0;
  }
  case SNDCTL_DSP_GETCAPS:
  {
    int *caps = (int *)arg;
    if (!caps)
      return -14;
    *caps = DSP_CAP_TRIGGER | DSP_CAP_OUTPUT;
    return 0;
  }
  case SNDCTL_DSP_SETTRIGGER:
  {
    int *tr = (int *)arg;
    if (!tr)
      return -14;
    hal_irq_disable();
    hda_trigger = *tr;
    if ((*tr & PCM_ENABLE_OUTPUT) && !hda_is_playing) {
      hda_start_stream_locked();
    }
    hal_irq_enable();
    return 0;
  }
  case SNDCTL_DSP_GETTRIGGER:
  {
    int *tr = (int *)arg;
    if (!tr)
      return -14;
    *tr = hda_trigger;
    return 0;
  }
  case SNDCTL_DSP_SPEED: {
    uint32_t *rate = (uint32_t *)arg;
    if (!rate)
      return -14;
    hal_irq_disable();
    if (*rate != hda_sample_rate && !hda_is_playing) {
      ring_head = ring_tail = ring_count = 0;
      hda_next_fill_slot = 0;
      hda_sample_rate = *rate;
    }
    hal_irq_enable();
    *rate = hda_sample_rate;
    return 0;
  }
  case SNDCTL_DSP_CHANNELS: {
    int *ch = (int *)arg;
    if (!ch)
      return -14;
    hal_irq_disable();
    if ((uint8_t)*ch != hda_channels && !hda_is_playing) {
      ring_head = ring_tail = ring_count = 0;
      hda_next_fill_slot = 0;
      hda_channels = (uint8_t)*ch;
    }
    hal_irq_enable();
    *ch = hda_channels;
    return 0;
  }
  case SNDCTL_DSP_SETFMT: {
    int *fmt = (int *)arg;
    if (!fmt)
      return -14;
    uint8_t new_bits = hda_bits;
    if (*fmt == AFMT_S16_LE)
      new_bits = 16;
    else if (*fmt == AFMT_U8)
      new_bits = 8;
    hal_irq_disable();
    if (new_bits != hda_bits && !hda_is_playing) {
      ring_head = ring_tail = ring_count = 0;
      hda_next_fill_slot = 0;
      hda_bits = new_bits;
    }
    hal_irq_enable();
    *fmt = (hda_bits == 16) ? AFMT_S16_LE : AFMT_U8;
    return 0;
  }
  case SNDCTL_DSP_STEREO: {
    int *st = (int *)arg;
    if (!st)
      return -14;
    uint8_t new_ch = (*st) ? 2 : 1;
    hal_irq_disable();
    if (new_ch != hda_channels) {
      hda_stop_stream_locked();
      ring_head = ring_tail = ring_count = 0;
      hda_next_fill_slot = 0;
      hda_channels = new_ch;
    }
    hal_irq_enable();
    return 0;
  }
  default:
    return -25;
  }
}

static int hda_vfs_poll(struct vfs_node *node, int events) {
  (void)node;
  if (!hda_present)
    return 0;

  int revents = 0;
  hal_irq_disable();
  uint32_t cnt = ring_count;
  hal_irq_enable();

  if ((events & (POLLOUT | POLLWRNORM)) && cnt < HDA_RING_SIZE) {
    revents |= (events & (POLLOUT | POLLWRNORM));
  }
  return revents;
}

extern void fb_register_device_node(const char *name, vfs_node_t *node);

void hda_register_vfs(void) {
  if (!hda_present || hda_ring)
    return; // Already initialized

  hda_ring = kmalloc(HDA_RING_SIZE);
  memset(hda_ring, 0, HDA_RING_SIZE);

  // Setup VFS DMA resources
  // BDL needs 1 page (4KB) for the descriptor entries
  uint64_t bdl_phys = (uint64_t)pmm_alloc_pages_constrained(1, 0xFFFFFFFF);
  hda_vfs_bdl_phys = bdl_phys;
  hda_vfs_bdl = (struct hda_bdl_entry *)(bdl_phys + pmm_get_hhdm_offset());

  // Audio buffers: HDA_VFS_NUM_BDL * HDA_VFS_BUF_SIZE bytes total
  // = 4 * 16384 = 65536 bytes = 16 pages
  uint32_t buf_pages = (HDA_VFS_TOTAL_SIZE + 4095) / 4096;
  uint64_t buf_phys =
      (uint64_t)pmm_alloc_pages_constrained(buf_pages, 0xFFFFFFFF);
  hda_vfs_buf_phys = buf_phys;
  hda_vfs_buf = (uint8_t *)(buf_phys + pmm_get_hhdm_offset());

  memset(hda_vfs_buf, 0, HDA_VFS_TOTAL_SIZE);

  // Setup BDL entries for all buffers
  for (int i = 0; i < HDA_VFS_NUM_BDL; i++) {
    uint64_t entry_phys = buf_phys + (i * HDA_VFS_BUF_SIZE);
    hda_vfs_bdl[i].addr_low = (uint32_t)entry_phys;
    hda_vfs_bdl[i].addr_high = (uint32_t)(entry_phys >> 32);
    hda_vfs_bdl[i].length = HDA_VFS_BUF_SIZE;
    hda_vfs_bdl[i].flags = 1; // IOC on every buffer completion
  }

  // Register node
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  vfs_node_init(node);
  strcpy(node->name, "hda_audio");
  node->flags = FS_CHARDEV;
  node->mask = 0666;
  node->write = hda_vfs_write;
  node->ioctl = hda_ioctl;
  node->poll = hda_vfs_poll;

  fb_register_device_node("hda_audio", node);
  klog_puts("[HDA] Registered /dev/hda_audio\n");
}
