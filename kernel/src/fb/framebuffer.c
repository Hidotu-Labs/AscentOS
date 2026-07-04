#include "framebuffer.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../drivers/input/keyboard.h"
#include "../drivers/pty.h"
#include "../font/font.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../syscalls/syscall.h"
#include "terminal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// Device Node Registry
// Keeps track of character device nodes so they persist across lookups
#define MAX_DEVICES 32

typedef struct {
  char name[64];
  vfs_node_t *node;
} device_entry_t;

static device_entry_t device_registry[MAX_DEVICES];
static int device_count = 0;

// Display Backend Selection
static fb_backend_t display_backend = FB_BACKEND_LIMINE; // Default fallback
static bool drm_available = false;

// Register a device node in the registry
void fb_register_device_node(const char *name, vfs_node_t *node) {
  if (!node)
    return;

  // Mark node as persistent so it doesn't get kfree'd during path resolution
  node->flags |= FS_PERSISTENT;

  // Check if device already exists and update it
  for (int i = 0; i < device_count; i++) {
    if (strcmp(device_registry[i].name, name) == 0) {
      device_registry[i].node = node;
      goto mount_vfs;
    }
  }
  // Add new entry
  if (device_count >= MAX_DEVICES)
    return;
  strncpy(device_registry[device_count].name, name, 63);
  device_registry[device_count].name[63] = '\0';
  device_registry[device_count].node = node;
  device_count++;

mount_vfs:
  // Also mount it in /dev so it appears in ls
  if (fs_root) {
    vfs_node_t *dev_dir = vfs_resolve_path("/dev");
    if (dev_dir) {
      klog_puts("[VFS] Registering '");
      klog_puts((char *)name);
      klog_puts("' in /dev\n");
      ramfs_mount_node(dev_dir, node);
    } else {
      klog_puts("[VFS] Warning: /dev not found during registration of '");
      klog_puts((char *)name);
      klog_puts("'\n");
    }
  }
}

// Look up a device in the registry
vfs_node_t *fb_lookup_device(const char *name) {
  for (int i = 0; i < device_count; i++) {
    if (strcmp(device_registry[i].name, name) == 0) {
      return device_registry[i].node;
    }
  }
  return NULL;
}

/**
 * fb_try_drm_device() - Detect and attempt to use DRM for display
 * Returns: true if DRM device is available and can be used, false otherwise
 * Note: This should be called after /dev is mounted and populated
 */
static bool fb_try_drm_device(void) {
  // Try to locate DRM device node at /dev/dri/card0
  vfs_node_t *drm_device = vfs_resolve_path("/dev/dri/card0");
  if (!drm_device) {
    klog_puts("[FB] DRM device /dev/dri/card0 not found, using framebuffer "
              "fallback\n");
    return false;
  }

  klog_puts("[FB] DRM device detected at /dev/dri/card0\n");
  return true;
}

fb_backend_t fb_get_backend(void) { return display_backend; }

const char *fb_get_backend_name(void) {
  switch (display_backend) {
  case FB_BACKEND_DRM:
    return "DRM";
  case FB_BACKEND_LIMINE:
  default:
    return "Limine Framebuffer";
  }
}

/**
 * fb_detect_drm_backend() - Detect DRM after it's been registered
 * Call this after drm_register_vfs() to switch to DRM if available
 */
void fb_detect_drm_backend(void) {
  if (fb_try_drm_device()) {
    display_backend = FB_BACKEND_DRM;
    drm_available = true;
    klog_puts("[FB] Display backend switched to: DRM\n");
  } else {
    klog_puts("[FB] DRM not available, keeping: Limine Framebuffer\n");
  }
}

static struct limine_framebuffer fb_local;
static struct limine_framebuffer *fb = NULL;
static void *backbuffer = NULL;
static size_t current_backbuffer_size = 0;
static volatile bool backbuffer_enabled = false;

// X11 double buffering - separate backbuffer for graphics mode
static void *x11_backbuffer = NULL;
static uint32_t x11_yoffset = 0; // Virtual Y offset for panning

// Dirty region tracking - bounding box of changed pixels
static uint32_t dirty_x1 = UINT32_MAX;
static uint32_t dirty_y1 = UINT32_MAX;
static uint32_t dirty_x2 = 0;
static uint32_t dirty_y2 = 0;
static bool dirty_valid = false;

void fb_mark_dirty(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  if (!backbuffer_enabled)
    return;

  if (x < dirty_x1)
    dirty_x1 = x;
  if (y < dirty_y1)
    dirty_y1 = y;
  if (x + w > dirty_x2)
    dirty_x2 = x + w;
  if (y + h > dirty_y2)
    dirty_y2 = y + h;
  dirty_valid = true;
}

void fb_init(struct limine_framebuffer *framebuffer) {
  if (!framebuffer)
    return;

  // Initialize with Limine framebuffer as default
  // DRM will be detected later in fb_register_vfs() after /dev is mounted
  display_backend = FB_BACKEND_LIMINE;
  drm_available = false;

  memcpy(&fb_local, framebuffer, sizeof(struct limine_framebuffer));
  fb = &fb_local;

  klog_puts("[FB] Initializing Framebuffer (early):\n");
  klog_puts("     Resolution: ");
  klog_uint64(fb->width);
  klog_puts("x");
  klog_uint64(fb->height);
  klog_puts("\n");
  klog_puts("     Pitch:      ");
  klog_uint64(fb->pitch);
  klog_puts("\n");
  klog_puts("     BPP:        ");
  klog_uint64(fb->bpp);
  klog_puts("\n");
  klog_puts("     Address:    ");
  klog_hex64((uint64_t)fb->address);
  klog_puts("\n");
  klog_puts("     Red:        size=");
  klog_uint64(fb->red_mask_size);
  klog_puts(" shift=");
  klog_uint64(fb->red_mask_shift);
  klog_puts("\n");
  klog_puts("     Green:      size=");
  klog_uint64(fb->green_mask_size);
  klog_puts(" shift=");
  klog_uint64(fb->green_mask_shift);
  klog_puts("\n");
  klog_puts("     Blue:       size=");
  klog_uint64(fb->blue_mask_size);
  klog_puts(" shift=");
  klog_uint64(fb->blue_mask_shift);
  klog_puts("\n");

  uint64_t fb_size = (uint64_t)fb->height * fb->pitch;

  // Re-map the virtual address provided by Limine with strict PCD/PWT
  // only if VMM is initialized.
  extern bool vmm_initialized;
  if (vmm_initialized) {
    uint64_t virt = (uint64_t)fb->address;
    uint64_t phys = virt - pmm_get_hhdm_offset();
    uint64_t *pml4 = vmm_get_active_pml4();
    uint64_t num_pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t flags = PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_PWT |
                     PAGE_FLAG_PCD | PAGE_FLAG_PAT;

    for (uint64_t i = 0; i < num_pages; i++) {
      vmm_map_page(pml4, virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
    }

    // TLB flush is already handled by vmm_map_page, but we do a full CR3 reload
    // for safety.
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");

    klog_puts("[FB] Write-Combining (WC) enabled via PAT.\n");

    // Clear hardware framebuffer
    if (fb->address) {
      volatile uint32_t *dest = (volatile uint32_t *)fb->address;
      for (uint32_t i = 0; i < fb_size / 4; i++) {
        dest[i] = 0x00000000;
      }
    }
  }

  // Allocate backbuffer and X11 buffer only if heap is ready.
  extern bool heap_initialized;
  if (heap_initialized) {
    if (!backbuffer || current_backbuffer_size < fb_size) {
      if (backbuffer)
        kfree(backbuffer);
      backbuffer = kmalloc(fb_size);
      if (backbuffer) {
        current_backbuffer_size = fb_size;
        memset(backbuffer, 0x00, fb_size);
      }
    }

    if (!x11_backbuffer) {
      x11_backbuffer = kmalloc(fb_size);
      if (x11_backbuffer) {
        memset(x11_backbuffer, 0x00, fb_size);
        klog_puts("[FB] X11 double buffer allocated\n");
      }
    }
  }
}

void fb_set_backbuffer_mode(bool enabled) { backbuffer_enabled = enabled; }
bool fb_is_backbuffer_enabled(void) { return backbuffer_enabled; }
void *fb_get_backbuffer(void) { return backbuffer; }

void fb_swap_buffer(void) {
  if (!backbuffer || fb_get_kd_mode() == KD_GRAPHICS)
    return;

  // No dirty region - nothing to swap
  if (!dirty_valid)
    return;

  // Clamp dirty region to screen bounds
  if (dirty_x1 >= fb->width || dirty_y1 >= fb->height) {
    dirty_valid = false;
    return;
  }

  uint32_t x1 = dirty_x1;
  uint32_t y1 = dirty_y1;
  uint32_t x2 = (dirty_x2 < fb->width) ? dirty_x2 : fb->width;
  uint32_t y2 = (dirty_y2 < fb->height) ? dirty_y2 : fb->height;

  // Reset dirty region for next frame
  dirty_x1 = UINT32_MAX;
  dirty_y1 = UINT32_MAX;
  dirty_x2 = 0;
  dirty_y2 = 0;
  dirty_valid = false;

  // Copy only the dirty region scanlines
  uint32_t copy_width = x2 - x1;
  if (copy_width == 0)
    return;

  for (uint32_t y = y1; y < y2; y++) {
    uint8_t *src = (uint8_t *)backbuffer + y * fb->pitch + x1 * 4;
    uint8_t *dst = (uint8_t *)fb->address + y * fb->pitch + x1 * 4;
    memcpy_to_wc(dst, src, copy_width * 4);
  }
  __asm__ volatile("sfence" ::: "memory");
}

// /dev/fb0 VFS node

static uint32_t fb_vfs_write(struct vfs_node *node, uint32_t offset,
                             uint32_t size, uint8_t *buffer) {
  (void)node;
  if (!fb || !backbuffer)
    return 0;
  uint32_t fb_size = fb->height * fb->pitch;

  if (offset >= fb_size)
    return 0;
  if (offset + size > fb_size)
    size = fb_size - offset;

  if (fb_get_kd_mode() == KD_GRAPHICS) {
    // In graphics mode, write to X11 backbuffer (double buffering)
    if (x11_backbuffer) {
      memcpy((uint8_t *)x11_backbuffer + offset, buffer, size);
    }
    return size;
  }

  // Write directly to framebuffer for text mode
  memcpy_to_wc((uint8_t *)fb->address + offset, buffer, size);
  __asm__ volatile("sfence" ::: "memory");

  return size;
}

// Framebuffer mmap: map physical fb memory directly into user space
// ───────── This allows apps like Doom to write directly without syscalls per
// frame.
#define FB_MMAP_PROT_READ 0x1
#define FB_MMAP_PROT_WRITE 0x2
#define FB_MMAP_PROT_EXEC 0x4
#define FB_MMAP_MAP_SHARED 0x01
#define FB_MMAP_MAP_PRIVATE 0x02
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOPAN_DISPLAY 0x4606

static uint64_t fb_vfs_mmap(struct vfs_node *node, uint64_t addr,
                            uint64_t length, uint64_t prot, uint64_t flags,
                            uint64_t offset) {
  (void)offset;
  (void)node;
  (void)addr;
  klog_puts("\n[FB_MMAP] length=");
  klog_uint64(length);
  klog_puts("\n");

  if (!fb)
    return (uint64_t)-1; // MAP_FAILED

  uint32_t fb_size = fb->height * fb->pitch;

  // Length must not exceed framebuffer size
  if (length == 0 || length > fb_size) {
    klog_puts("[FB_MMAP] Error: invalid length\n");
    return (uint64_t)-1;
  }

  // Must be shared mapping for direct framebuffer access
  if (!(flags & FB_MMAP_MAP_SHARED)) {
    klog_puts("[FB_MMAP] Error: only MAP_SHARED supported for framebuffer\n");
    return (uint64_t)-1;
  }

  // In graphics mode (X11), map the X11 backbuffer for double buffering
  // In text mode, map the hardware framebuffer directly
  // FORCE direct mapping of hardware framebuffer.
  // The X11 double buffering logic is broken and results in a black screen.
  void *buffer_to_map = fb->address;
  klog_puts("[FB_MMAP] Mapping hardware framebuffer (direct mode) FORCED\n");

// Allocate a virtual address range in user space
// Use the mmap bump allocator from sys_mm.c
#define PAGE_SIZE 4096
#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

  uint64_t aligned_len = PAGE_ALIGN_UP(length);
  uint64_t vaddr = addr;
  if (vaddr == 0) {
    vaddr = mm_alloc_mmap_region(aligned_len);
  }
  if (vaddr == 0) {
    klog_puts("[FB_MMAP] Error: mmap region exhausted\n");
    return (uint64_t)-1;
  }

  // Build page flags from prot - Using PCD|PWT for strict Uncacheable (UC)
  // For X11 backbuffer (heap memory), use WB (write-back) caching for speed
  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER;
  if (buffer_to_map == fb->address) {
    // Hardware framebuffer - use Write-Combining (WC) via PAT
    page_flags |= (PAGE_FLAG_PWT | PAGE_FLAG_PCD | PAGE_FLAG_PAT);
  }
  if (prot & FB_MMAP_PROT_WRITE)
    page_flags |= PAGE_FLAG_RW;
  if (!(prot & FB_MMAP_PROT_EXEC))
    page_flags |= PAGE_FLAG_NX;

  // Resolve physical address of the buffer to map.
  // For the hardware framebuffer, Limine always provides an HHDM-mapped
  // address, so we can compute the physical address directly via arithmetic.
  // This avoids fragile page-table walking that can fail when QEMU's PCI
  // layout shifts (e.g. adding -usb).  For heap buffers (x11_backbuffer),
  // fall back to page-table walking.
  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t real_phys;
  if (buffer_to_map == fb->address) {
    // Hardware framebuffer — always HHDM-mapped by Limine
    real_phys = (uint64_t)buffer_to_map - pmm_get_hhdm_offset();
  } else {
    // Heap buffer (x11_backbuffer) — walk page tables
    real_phys = vmm_virt_to_phys(pml4, (uint64_t)buffer_to_map);
  }
  if (real_phys == 0) {
    klog_puts("[FB_MMAP] Error: failed to resolve physical address\n");
    return (uint64_t)-1;
  }

  uint64_t phys_offset = real_phys & (PAGE_SIZE - 1);
  uint64_t aligned_phys = real_phys & PAGE_MASK;
  uint64_t num_pages = (length + phys_offset + PAGE_SIZE - 1) / PAGE_SIZE;

  for (uint64_t i = 0; i < num_pages; i++) {
    uint64_t phys_page = aligned_phys + i * PAGE_SIZE;
    uint64_t virt_page = vaddr + i * PAGE_SIZE;

    if (!vmm_map_page(pml4, virt_page, phys_page, page_flags)) {
      klog_puts("[FB_MMAP] Error: vmm_map_page failed\n");
      return (uint64_t)-1;
    }
    vmm_flush_tlb(virt_page);
  }

  klog_puts("[FB_MMAP] Mapped buffer at ");
  klog_uint64(vaddr + phys_offset);
  klog_puts("\n");

  return vaddr + phys_offset;
}

static uint32_t fb_vfs_read(struct vfs_node *node, uint32_t offset,
                            uint32_t size, uint8_t *buffer) {
  (void)node;
  if (!fb || !backbuffer)
    return 0;
  uint32_t fb_size = fb->height * fb->pitch;

  if (offset >= fb_size)
    return 0;
  if (offset + size > fb_size)
    size = fb_size - offset;

  uint8_t *src =
      backbuffer_enabled ? (uint8_t *)backbuffer : (uint8_t *)fb->address;
  memcpy(buffer, src + offset, size);

  return size;
}

// /dev/console VFS node

static uint8_t canon_buffer[1024];
static uint32_t canon_len = 0;
static uint32_t canon_pos = 0;
static uint32_t console_pgid = 0;

static void console_vfs_open(vfs_node_t *node) {
  (void)node;
  struct thread *t = sched_get_current();
  if (t && console_pgid == 0) {
    console_pgid = t->pgid;
  }
}
static void console_vfs_close(vfs_node_t *node) { (void)node; }

static uint32_t console_vfs_read(struct vfs_node *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  if (size == 0)
    return 0;

  // Helper: check ISIG and send signal for a character.
  // Returns true if the character was consumed as a signal (don't buffer it).
  extern void signal_send_pgid(uint32_t pgid, int sig);
#define CONSOLE_CHECK_ISIG(c)                                                  \
  do {                                                                         \
    if (console_termios.c_lflag & ISIG) {                                      \
      if ((uint8_t)(c) == console_termios.c_cc[0] && console_pgid != 0) {      \
        signal_send_pgid(console_pgid, 2); /* SIGINT */                        \
        return (uint32_t)-4;               /* EINTR */                         \
      }                                                                        \
      if ((uint8_t)(c) == console_termios.c_cc[1] && console_pgid != 0) {      \
        signal_send_pgid(console_pgid, 3); /* SIGQUIT */                       \
        return (uint32_t)-4;               /* EINTR */                         \
      }                                                                        \
    }                                                                          \
  } while (0)

  if (console_termios.c_lflag & ICANON) {
    // If we have data in the canon buffer, return it first
    if (canon_pos < canon_len) {
      uint32_t to_copy = canon_len - canon_pos;
      if (to_copy > size)
        to_copy = size;
      memcpy(buffer, canon_buffer + canon_pos, to_copy);
      canon_pos += to_copy;
      if (canon_pos == canon_len) {
        canon_pos = 0;
        canon_len = 0;
      }
      return to_copy;
    }

    // Otherwise, collect a new line
    canon_len = 0;
    canon_pos = 0;

    while (1) {
      char c = keyboard_get_char();

      // ICRNL: Map CR to NL on input
      if (c == '\r' && (console_termios.c_iflag & ICRNL))
        c = '\n';

      // ISIG: check for signal-generating characters (VINTR, VQUIT)
      CONSOLE_CHECK_ISIG(c);

      // Handle erasing (Backspace or Delete)
      if (c == '\b' || c == 0x7F) {
        if (canon_len > 0) {
          canon_len--;
          if (console_termios.c_lflag & ECHO) {
            console_putchar('\b');
          }
        }
        continue;
      }

      // Buffer the character
      if (canon_len < sizeof(canon_buffer)) {
        canon_buffer[canon_len++] = c;
        if (console_termios.c_lflag & ECHO) {
          console_putchar(c);
        }
      }

      // If it's a newline, we have a complete line
      if (c == '\n')
        break;
    }

    // Return as much as requested from the newly collected line
    uint32_t to_copy = canon_len;
    if (to_copy > size)
      to_copy = size;
    memcpy(buffer, canon_buffer, to_copy);
    canon_pos = to_copy;

    if (canon_pos == canon_len) {
      canon_pos = 0;
      canon_len = 0;
    }
    return to_copy;
  } else {
    // Non-canonical mode (raw-ish)
    uint32_t count = 0;
    while (count < size) {
      char c = keyboard_get_char();

      // ICRNL: Map CR to NL on input
      if (c == '\r' && (console_termios.c_iflag & ICRNL))
        c = '\n';

      // ISIG: check for signal-generating characters (VINTR, VQUIT)
      CONSOLE_CHECK_ISIG(c);

      // ECHO: Echo input characters
      if (console_termios.c_lflag & ECHO) {
        console_putchar(c);
      }

      buffer[count++] = (uint8_t)c;

      if (!keyboard_has_char())
        break;
    }
    return count;
  }
#undef CONSOLE_CHECK_ISIG
}

static int console_vfs_poll(struct vfs_node *node, int events) {
  (void)node;
  int revents = 0;
  if (events & POLLIN) {
    if (keyboard_has_char()) {
      revents |= POLLIN;
    }
  }
  if (events & POLLOUT) {
    revents |= POLLOUT; // Console is always ready to write
  }
  return revents;
}

// Use console_write_batch so that each VFS write() call results in exactly
// ONE backbuffer swap — eliminating per-character flicker for apps like kilo.
static uint32_t console_vfs_write(struct vfs_node *node, uint32_t offset,
                                  uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;

  if ((console_termios.c_oflag & OPOST) && (console_termios.c_oflag & ONLCR)) {
    fb_set_backbuffer_mode(true);
    // ONLCR: Map NL to CR-NL on output
    for (uint32_t i = 0; i < size; i++) {
      if (buffer[i] == '\n') {
        console_putchar('\r');
      }
      console_putchar(buffer[i]);
    }
    fb_swap_buffer();
    fb_set_backbuffer_mode(false);
  } else {
    console_write_batch((const char *)buffer, size);
  }
  return size;
}

// /dev/null - discard all writes, return EOF on read
static uint32_t null_vfs_read(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)size;
  (void)buffer;
  return 0; // EOF
}

static uint32_t null_vfs_write(struct vfs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)buffer;
  return size; // Discard but report success
}

// /dev/zero - return zeros on read, discard writes
static uint32_t zero_vfs_read(struct vfs_node *node, uint32_t offset,
                              uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  memset(buffer, 0, size);
  return size;
}

static uint32_t zero_vfs_write(struct vfs_node *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
  (void)node;
  (void)offset;
  (void)buffer;
  return size; // Discard but report success
}

// Helper for device registration
// Character devices are always created as virtual in-memory nodes, not
// persisted to ext2
static void setup_chardev(
    vfs_node_t *dev_dir, const char *name,
    uint32_t (*read_fn)(struct vfs_node *, uint32_t, uint32_t, uint8_t *),
    uint32_t (*write_fn)(struct vfs_node *, uint32_t, uint32_t, uint8_t *),
    void (*open_fn)(struct vfs_node *), void (*close_fn)(struct vfs_node *),
    int (*poll_fn)(struct vfs_node *, int),
    int (*ioctl_fn)(struct vfs_node *, uint32_t, uint64_t),
    uint64_t (*mmap_fn)(struct vfs_node *, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t),
    void *device, uint32_t length, uint32_t rdev) {

  // Create virtual device node
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return;

  vfs_node_init(node);
  strncpy(node->name, name, 127);
  node->flags = FS_CHARDEV | FS_PERSISTENT;
  node->mask = 0666;
  node->length = length;
  node->device = device;
  node->read = read_fn;
  node->write = write_fn;
  node->open = open_fn;
  node->close = close_fn;
  node->poll = poll_fn;
  node->ioctl = ioctl_fn;
  node->mmap = mmap_fn;
  node->inode = rdev;

  // Register in device registry for persistent lookups
  fb_register_device_node(name, node);

  // Also mount it in the VFS directory so it appears in ls
  if (dev_dir) {
    ramfs_mount_node(dev_dir, node);
  }
}

uint16_t fb_get_bpp(void) { return fb ? fb->bpp : 0; }

static int fb_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  (void)node;
  if (!fb)
    return -1;

  /*
    klog_puts("[FB_IOCTL] request=0x");
    klog_hex32(request);
    klog_puts("\n");
  */

  switch (request) {
  case FBIOGET_VSCREENINFO: {
    struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
    memset(var, 0, sizeof(struct fb_var_screeninfo));
    var->xres = (uint32_t)fb->width;
    var->yres = (uint32_t)fb->height;
    var->xres_virtual = (uint32_t)(fb->pitch / (fb->bpp / 8));
    var->yres_virtual = (uint32_t)fb->height;
    var->bits_per_pixel = 32; // !!! FORCE 32-BIT !!!
    klog_puts("[!!! FB_VSCREENINFO !!!] Forced 32bpp\n");

    var->red.length = fb->red_mask_size;
    var->red.offset = fb->red_mask_shift;
    var->green.length = fb->green_mask_size;
    var->green.offset = fb->green_mask_shift;
    var->blue.length = fb->blue_mask_size;
    var->blue.offset = fb->blue_mask_shift;

    // For 32-bit framebuffer, explicitly specify the 8-bit transparency/alpha
    // padding
    if (fb->bpp == 32) {
      var->transp.length = 8;
      // Assume alpha is the remaining byte not used by RGB
      uint32_t used_mask = ((1 << fb->red_mask_size) - 1) << fb->red_mask_shift;
      used_mask |= ((1 << fb->green_mask_size) - 1) << fb->green_mask_shift;
      used_mask |= ((1 << fb->blue_mask_size) - 1) << fb->blue_mask_shift;

      if ((used_mask & 0xFF000000) == 0)
        var->transp.offset = 24;
      else if ((used_mask & 0x000000FF) == 0)
        var->transp.offset = 0;
      else
        var->transp.offset = 24; // Default fallback
    }

    return 0;
  }
  case FBIOGET_FSCREENINFO: {
    struct fb_fix_screeninfo *fix = (struct fb_fix_screeninfo *)arg;
    memset(fix, 0, sizeof(struct fb_fix_screeninfo));
    strcpy(fix->id, "ascentos-fb");

    // Resolve exact physical address
    uint64_t phys =
        vmm_virt_to_phys(vmm_get_active_pml4(), (uint64_t)fb->address);
    fix->smem_start = (unsigned long)phys;
    fix->smem_len = (uint32_t)(fb->height * fb->pitch);
    fix->line_length = (uint32_t)fb->pitch;
    fix->visual = 2; // FB_VISUAL_TRUECOLOR
    fix->accel = 0;  // No hardware acceleration
    return 0;
  }
  case FBIOPUT_VSCREENINFO: {
    // X11 often calls this to probe or 'confirm' mode switching.
    // Since we don't support dynamic resolution or bpp switching,
    // we MUST firmly overwrite the user's struct with our fixed
    // hardware configuration and return success. This informs the
    // fbdev driver of the clamped/enforced parameters.
    struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;

    var->xres = (uint32_t)fb->width;
    var->yres = (uint32_t)fb->height;
    var->xres_virtual = (uint32_t)(fb->pitch / (fb->bpp / 8));
    var->yres_virtual = (uint32_t)fb->height;
    var->bits_per_pixel = (uint32_t)fb->bpp;

    var->red.length = fb->red_mask_size;
    var->red.offset = fb->red_mask_shift;
    var->green.length = fb->green_mask_size;
    var->green.offset = fb->green_mask_shift;
    var->blue.length = fb->blue_mask_size;
    var->blue.offset = fb->blue_mask_shift;

    if (fb->bpp == 32) {
      var->transp.length = 8;
      uint32_t used_mask = ((1 << fb->red_mask_size) - 1) << fb->red_mask_shift;
      used_mask |= ((1 << fb->green_mask_size) - 1) << fb->green_mask_shift;
      used_mask |= ((1 << fb->blue_mask_size) - 1) << fb->blue_mask_shift;
      if ((used_mask & 0xFF000000) == 0)
        var->transp.offset = 24;
      else if ((used_mask & 0x000000FF) == 0)
        var->transp.offset = 0;
      else
        var->transp.offset = 24;
    }

    return 0;
  }
  case FBIOPAN_DISPLAY: {
    // Page flip: copy X11 backbuffer to hardware framebuffer
    // This is called by X11 after rendering a frame to the mmap'd buffer
    if (!x11_backbuffer || !fb) {
      return -1;
    }

    uint32_t fb_size = fb->height * fb->pitch;

    // Copy entire X11 backbuffer to hardware framebuffer
    // This is the "page flip" - atomic swap of entire frame
    memcpy_to_wc((uint8_t *)fb->address, (uint8_t *)x11_backbuffer, fb_size);
    __asm__ volatile("sfence" ::: "memory");

    // Update yoffset if requested (for virtual screen panning)
    struct fb_var_screeninfo *var = (struct fb_var_screeninfo *)arg;
    if (var) {
      x11_yoffset = var->yoffset;
    }

    return 0;
  }
  case 0x4611: { // FBIO_WAITFORVSYNC
    // Minimal implementation: just succeed to keep X11 happy
    return 0;
  }
  default:
    return -1;
  }
}

// /dev/tty0 VT ioctls for Xfbdev/Xorg
// Linux VT ioctl numbers
#define VT_OPENQRY 0x5600
#define VT_GETMODE 0x5601
#define VT_SETMODE 0x5602
#define VT_GETSTATE 0x5603
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608
// Removed KDGETMODE redefinition
#define KDSETMODE 0x4B3A
#define KDGKBMODE 0x4B44
#define KDSKBMODE 0x4B45
#define VT_RELDISP 0x5605

#define K_RAW 0x00
#define K_XLATE 0x01
#define K_MEDIUMRAW 0x02

struct vt_stat {
  uint16_t v_active; // Active VT
  uint16_t v_signal; // Signal to send
  uint16_t v_state;  // VT bitmask of open VTs
};

struct vt_mode {
  char mode;    // VT mode (VT_AUTO, VT_PROCESS)
  char waitv;   // if set, wait for release
  short relsig; // signal to send on release
  short acqsig; // signal to send on acquire
  short frsig;  // unused
};

static volatile int current_kd_mode = KD_TEXT;
static int current_kb_mode = K_XLATE;

static int tty0_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  (void)node;

  switch (request) {
  case VT_OPENQRY: {
    // Return next available VT number
    int *vt = (int *)arg;
    if (vt)
      *vt = 1;
    return 0;
  }
  case VT_GETSTATE: {
    struct vt_stat *vs = (struct vt_stat *)arg;
    if (!vs)
      return -14;
    vs->v_active = 1; // VT 1 is active
    vs->v_signal = 0;
    vs->v_state = 0x02; // VT 1 is open (bit 1)
    return 0;
  }
  case VT_GETMODE: {
    struct vt_mode *vm = (struct vt_mode *)arg;
    if (!vm)
      return -14;
    vm->mode = 0; // VT_AUTO
    vm->waitv = 0;
    vm->relsig = 0;
    vm->acqsig = 0;
    vm->frsig = 0;
    return 0;
  }
  case VT_SETMODE: {
    // Accept mode change silently (VT_AUTO/VT_PROCESS)
    return 0;
  }
  case VT_ACTIVATE: {
    // Single-console OS — just succeed
    return 0;
  }
  case VT_WAITACTIVE: {
    // We're always on VT 1, so always active
    return 0;
  }
  case VT_RELDISP: {
    return 0;
  }
  case VT_DISALLOCATE: {
    // Single-console OS — nothing to deallocate, just succeed
    return 0;
  }
  case KDGETMODE: {
    int *mode = (int *)arg;
    if (mode)
      *mode = current_kd_mode;
    return 0;
  }
  case KDSETMODE: {
    fb_set_kd_mode((int)arg);
    return 0;
  }
  case KDGKBMODE: {
    int *mode = (int *)arg;
    if (mode)
      *mode = current_kb_mode;
    return 0;
  }
  case KDSKBMODE: {
    current_kb_mode = (int)arg;
    return 0;
  }
  case TIOCGPGRP: {
    int *pgid = (int *)arg;
    if (!pgid)
      return -14;
    *pgid = (int)console_pgid;
    return 0;
  }
  case TIOCSPGRP: {
    int *pgid = (int *)arg;
    if (!pgid)
      return -14;
    console_pgid = (uint32_t)*pgid;
    return 0;
  }
  case TIOCGWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws)
      return -14;
    ws->ws_row = (unsigned short)(fb_get_height() / FONT_HEIGHT);
    ws->ws_col = (unsigned short)(fb_get_width() / FONT_WIDTH);
    ws->ws_xpixel = (unsigned short)fb_get_width();
    ws->ws_ypixel = (unsigned short)fb_get_height();
    return 0;
  }
  case TIOCSWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws)
      return -14;
    // Window size is read-only in our implementation
    // but return success to avoid breaking applications
    return 0;
  }
  case TCGETS: {
    if (!arg)
      return -14;
    extern struct termios console_termios;
    struct kernel_termios kt;
    kt.c_iflag = console_termios.c_iflag;
    kt.c_oflag = console_termios.c_oflag;
    kt.c_cflag = console_termios.c_cflag;
    kt.c_lflag = console_termios.c_lflag;
    kt.c_line = console_termios.c_line;
    memcpy(kt.c_cc, console_termios.c_cc, KERNEL_NCCS);
    memcpy((void *)arg, &kt, sizeof(struct kernel_termios));
    return 0;
  }
  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    if (!arg)
      return -14;
    extern struct termios console_termios;
    struct kernel_termios kt;
    memcpy(&kt, (const void *)arg, sizeof(struct kernel_termios));
    console_termios.c_iflag = kt.c_iflag;
    console_termios.c_oflag = kt.c_oflag;
    console_termios.c_cflag = kt.c_cflag;
    console_termios.c_lflag = kt.c_lflag;
    console_termios.c_line = kt.c_line;
    memcpy(console_termios.c_cc, kt.c_cc, KERNEL_NCCS);
    return 0;
  }
  case TIOCGETD: {
    int *ldisc = (int *)arg;
    if (!ldisc)
      return -14;
    *ldisc = 0; // N_TTY
    return 0;
  }
  case TIOCSETD: {
    // Silently accept setting to N_TTY (0)
    int ldisc = (int)arg;
    if (ldisc != 0)
      return -22; // EINVAL
    return 0;
  }
  default:
    return -25; // ENOTTY
  }
}

// Registration

void fb_register_vfs(void) {
  if (!fs_root)
    return;

  vfs_node_t *dev_dir = vfs_finddir(fs_root, "dev");
  if (!dev_dir)
    return;

  uint32_t fb_size = fb->height * fb->pitch;

  // /dev/fb0 - Framebuffer device (always available as fallback)
  setup_chardev(dev_dir, "fb0", fb_vfs_read, fb_vfs_write, NULL, NULL, NULL,
                fb_ioctl, fb_vfs_mmap, fb, fb_size, (29 << 8) | 0);

  // /dev/console (Major 5, Minor 1)
  setup_chardev(dev_dir, "console", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll,
                tty0_ioctl, NULL, NULL, 0, (5 << 8) | 1);

  // /dev/tty (Major 5, Minor 0)
  setup_chardev(dev_dir, "tty", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll,
                tty0_ioctl, NULL, NULL, 0, (5 << 8) | 0);

  // /dev/stdin
  setup_chardev(dev_dir, "stdin", console_vfs_read, 0, console_vfs_open,
                console_vfs_close, console_vfs_poll, 0, NULL, NULL, 0,
                (0 << 8) | 0);

  // /dev/stdout
  setup_chardev(dev_dir, "stdout", 0, console_vfs_write, console_vfs_open,
                console_vfs_close, NULL, 0, NULL, NULL, 0, (0 << 8) | 1);

  // /dev/stderr
  setup_chardev(dev_dir, "stderr", 0, console_vfs_write, console_vfs_open,
                console_vfs_close, NULL, 0, NULL, NULL, 0, (0 << 8) | 2);

  // /dev/null (Major 1, Minor 3)
  setup_chardev(dev_dir, "null", null_vfs_read, null_vfs_write, NULL, NULL,
                NULL, NULL, NULL, NULL, 0, (1 << 8) | 3);

  // /dev/zero (Major 1, Minor 5)
  setup_chardev(dev_dir, "zero", zero_vfs_read, zero_vfs_write, NULL, NULL,
                NULL, NULL, NULL, NULL, 0, (1 << 8) | 5);

  // /dev/tty0 — virtual terminal device (Major 4, Minor 0)
  setup_chardev(dev_dir, "tty0", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll,
                tty0_ioctl, NULL, NULL, 0, (4 << 8) | 0);

  // /dev/tty1-tty7 — individual virtual terminals (Major 4, Minor 1+)
  setup_chardev(dev_dir, "tty1", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll,
                tty0_ioctl, NULL, NULL, 0, (4 << 8) | 1);
  setup_chardev(dev_dir, "tty2", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll,
                tty0_ioctl, NULL, NULL, 0, (4 << 8) | 2);
  setup_chardev(dev_dir, "tty3", console_vfs_read, console_vfs_write,
                console_vfs_open, console_vfs_close, console_vfs_poll,
                tty0_ioctl, NULL, NULL, 0, (4 << 8) | 3);

  // /dev/apm_bios
  setup_chardev(dev_dir, "apm_bios", zero_vfs_read, zero_vfs_write, NULL, NULL,
                NULL, NULL, NULL, NULL, 0, (10 << 8) | 134);

  // /dev/misc/apm_bios
  vfs_node_t *misc_dir = vfs_finddir(dev_dir, "misc");
  if (misc_dir) {
    setup_chardev(misc_dir, "apm_bios", zero_vfs_read, zero_vfs_write, NULL,
                  NULL, NULL, NULL, NULL, NULL, 0, (10 << 8) | 134);
  }

  // Register PTY devices (/dev/ptmx and /dev/pts/N)
  pty_register_devices();

  // Note: don't free dev_dir - it still points to a valid VFS node
}

// Direct drawing primitives

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (x >= fb->width || y >= fb->height)
    return;

  void *target = backbuffer_enabled ? backbuffer : fb->address;
  uint32_t *pixel = (uint32_t *)((uint8_t *)target + y * fb->pitch + x * 4);
  *pixel = color;
  fb_mark_dirty(x, y, 1, 1);
}

// Fast 32-bit fill for aligned regions
static inline void fill_scanline32(uint32_t *dst, uint32_t count,
                                   uint32_t color) {
  // Unroll for speed
  while (count >= 8) {
    dst[0] = color;
    dst[1] = color;
    dst[2] = color;
    dst[3] = color;
    dst[4] = color;
    dst[5] = color;
    dst[6] = color;
    dst[7] = color;
    dst += 8;
    count -= 8;
  }
  while (count--) {
    *dst++ = color;
  }
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color) {
  if (x >= fb->width || y >= fb->height)
    return;
  if (x + w > fb->width)
    w = fb->width - x;
  if (y + h > fb->height)
    h = fb->height - y;

  // Always update backbuffer if it exists to keep it in sync
  if (backbuffer) {
    for (uint32_t row = y; row < y + h; row++) {
      uint32_t *line =
          (uint32_t *)((uint8_t *)backbuffer + row * fb->pitch + x * 4);
      fill_scanline32(line, w, color);
    }
  }

  // Draw to hardware if backbuffer is disabled or not present
  if (!backbuffer_enabled) {
    for (uint32_t row = y; row < y + h; row++) {
      uint32_t *line =
          (uint32_t *)((uint8_t *)fb->address + row * fb->pitch + x * 4);
      fill_scanline32(line, w, color);
    }
  } else {
    fb_mark_dirty(x, y, w, h);
  }
}

void fb_clear(uint32_t color) {
  void *target = backbuffer_enabled ? backbuffer : fb->address;
  uint32_t total_pixels = fb->width * fb->height;
  uint32_t *pixels = (uint32_t *)target;
  fill_scanline32(pixels, total_pixels, color);
  fb_mark_dirty(0, 0, fb->width, fb->height);
}

// Draw a single glyph scanline (8 pixels) with fg/bg colors in one operation
// This replaces 8 individual fb_put_pixel calls per scanline
void fb_draw_glyph_scanline(uint32_t x, uint32_t y, uint8_t bits, uint32_t fg,
                            uint32_t bg) {
  if (x >= fb->width || y >= fb->height)
    return;

  // Always update backbuffer if it exists to keep it in sync
  if (backbuffer) {
    uint32_t *line =
        (uint32_t *)((uint8_t *)backbuffer + y * fb->pitch + x * 4);
    for (int i = 0; i < 8; i++) {
      line[i] = (bits & (0x80 >> i)) ? fg : bg;
    }
  }

  if (!backbuffer_enabled) {
    uint32_t *line =
        (uint32_t *)((uint8_t *)fb->address + y * fb->pitch + x * 4);
    for (int i = 0; i < 8; i++) {
      line[i] = (bits & (0x80 >> i)) ? fg : bg;
    }
  } else {
    // Mark dirty only when backbuffering is active
    fb_mark_dirty(x, y, 8, 1);
  }
}

void fb_set_kd_mode(int mode) {
  current_kd_mode = mode;
  if (current_kd_mode == KD_GRAPHICS) {
    // 1. Flush all caches to RAM
    __asm__ volatile("wbinvd" ::: "memory");

    // 2. Full TLB flush via CR3 reload
    {
      uint64_t cr3;
      __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
      __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
    }

    // 3. Clear the physical framebuffer via volatile access
    if (fb && fb->address) {
      volatile uint32_t *target = (volatile uint32_t *)fb->address;
      uint32_t size = (fb->height * fb->pitch) / 4;
      for (uint32_t i = 0; i < size; i++) {
        target[i] = 0x00000000;
      }
    }

    // 3. Final flush to ensure zeroing reached RAM
    __asm__ volatile("wbinvd" ::: "memory");
  }
}

uint32_t fb_get_width(void) { return fb->width; }
uint32_t fb_get_height(void) { return fb->height; }
int fb_get_kd_mode(void) { return current_kd_mode; }
void *fb_get_base(void) { return fb->address; }
uint32_t fb_get_pitch(void) { return fb->pitch; }
