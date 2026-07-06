// sys_ioctl.c — ioctl syscall
#include "../console/klog.h"
#include "../fb/framebuffer.h"
#include "../font/font.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include <stdint.h>

#ifndef IOCTL_DEBUG_LOGGING
#define IOCTL_DEBUG_LOGGING 0
#endif

struct termios console_termios;

static int ioctl_arg_is_scalar(uint32_t request) {
  switch (request) {
  case 0x40044590: // EVIOCGRAB: _IOW('E', 0x90, int)
  case 0x40044591: // EVIOCREVOKE: _IOW('E', 0x91, int)
    return 1;
  default:
    return 0;
  }
}

static uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
#if IOCTL_DEBUG_LOGGING
  if (t) {
    klog_puts("[IOCTL] tid=");
    klog_uint64(t->tid);
    klog_puts(" fd=");
    klog_uint64(fd);
    klog_puts(" request=0x");
    klog_hex32(request);
    klog_puts(" arg=0x");
    klog_uint64(arg);
    klog_puts("\n");
  }

  klog_puts("[SYSCALL] sys_ioctl ENTER fd=");
  klog_uint64(fd);
  klog_puts(" request=0x");
  klog_hex32((uint32_t)request);
  klog_puts(" arg=0x");
  klog_hex64(arg);
  klog_puts("\n");
#endif

  if (arg > USER_ADDR_MAX)
    return (uint64_t)-14;

  if (!t || fd >= MAX_FDS)
    return (uint64_t)-9;

  if (fd < MAX_FDS && t->fds[fd]) {
    vfs_node_t *node = t->fds[fd];
#if IOCTL_DEBUG_LOGGING
    klog_puts("[SYSCALL] ioctl: node name=");
    klog_puts(node->name);
    klog_puts(" has_ioctl=");
    klog_uint64(node->ioctl ? 1 : 0);
    klog_puts("\n");
#endif

    if (node->ioctl) {
      if (request & 0xC0000000) {
        size_t sz = (request >> 16) & 0x3FFF;
        if (sz > 0 && !ioctl_arg_is_scalar((uint32_t)request) &&
            !vmm_is_user_addr_range_valid(arg, sz)) {
          klog_puts(
              "[SYSCALL] ioctl: invalid arg pointer for encoded request\n");
          return (uint64_t)-14;
        }
      }
      uint64_t res = (uint64_t)node->ioctl(node, (uint32_t)request, arg);
#if IOCTL_DEBUG_LOGGING
      klog_puts("[SYSCALL] ioctl: node handler returned 0x");
      klog_hex64(res);
      klog_puts("\n");
#endif
      if (res != (uint64_t)-25)
        return res;
    }
  }

  uint64_t ret = 0;
  switch ((uint32_t)request) {
  case TIOCGWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws || !vmm_is_user_addr_range_valid(arg, sizeof(struct winsize))) {
      ret = (uint64_t)-14;
      break;
    }
    ws->ws_row = (unsigned short)(fb_get_height() / FONT_HEIGHT);
    ws->ws_col = (unsigned short)(fb_get_width() / FONT_WIDTH);
    ws->ws_xpixel = (unsigned short)fb_get_width();
    ws->ws_ypixel = (unsigned short)fb_get_height();
    ret = 0;
    break;
  }
  case TCGETS: {
    if (!arg ||
        !vmm_is_user_addr_range_valid(arg, sizeof(struct kernel_termios))) {
      ret = (uint64_t)-14;
      break;
    }
    struct kernel_termios kt;
    kt.c_iflag = console_termios.c_iflag;
    kt.c_oflag = console_termios.c_oflag;
    kt.c_cflag = console_termios.c_cflag;
    kt.c_lflag = console_termios.c_lflag;
    kt.c_line = console_termios.c_line;
    memcpy(kt.c_cc, console_termios.c_cc, KERNEL_NCCS);
    memcpy((void *)arg, &kt, sizeof(struct kernel_termios));
    ret = 0;
    break;
  }
  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    if (!arg ||
        !vmm_is_user_addr_range_valid(arg, sizeof(struct kernel_termios))) {
      ret = (uint64_t)-14;
      break;
    }
    struct kernel_termios kt;
    memcpy(&kt, (const void *)arg, sizeof(struct kernel_termios));
    console_termios.c_iflag = kt.c_iflag;
    console_termios.c_oflag = kt.c_oflag;
    console_termios.c_cflag = kt.c_cflag;
    console_termios.c_lflag = kt.c_lflag;
    console_termios.c_line = kt.c_line;
    memcpy(console_termios.c_cc, kt.c_cc, KERNEL_NCCS);
    ret = 0;
    break;
  }
  case 0x5470: { // KBDSCANMODE_GET
    int *mode = (int *)arg;
    if (!mode) {
      ret = (uint64_t)-14;
      break;
    }
    extern bool keyboard_is_scancode_mode(void);
    *mode = keyboard_is_scancode_mode() ? 1 : 0;
    ret = 0;
    break;
  }
  case 0x5471: { // KBDSCANMODE_SET
    int mode = (int)arg;
    extern void keyboard_set_scancode_mode(bool enabled);
    keyboard_set_scancode_mode(mode != 0);
    ret = 0;
    break;
  }
  case 0x5472: { // KBDSCANCODE_READ
    unsigned char *event = (unsigned char *)arg;
    if (!event) {
      ret = (uint64_t)-14;
      break;
    }
    extern bool keyboard_has_scancode(void);
    extern bool keyboard_get_scancode(void *event_ptr);
    if (keyboard_has_scancode()) {
      if (keyboard_get_scancode((void *)event)) {
        ret = 1;
        break;
      }
    }
    ret = 0;
    break;
  }
  case VT_OPENQRY: {
    int *vt = (int *)arg;
    if (!vt) {
      ret = (uint64_t)-14;
      break;
    }
    *vt = 1;
    ret = 0;
    break;
  }
  case VT_GETMODE: {
    struct vt_mode *vtm = (struct vt_mode *)arg;
    if (!vtm) {
      ret = (uint64_t)-14;
      break;
    }
    memset(vtm, 0, sizeof(struct vt_mode));
    ret = 0;
    break;
  }
  case VT_SETMODE:
    ret = 0;
    break;
  case VT_GETSTATE: {
    struct vt_stat *vts = (struct vt_stat *)arg;
    if (!vts) {
      ret = (uint64_t)-14;
      break;
    }
    memset(vts, 0, sizeof(struct vt_stat));
    vts->v_active = 1;
    vts->v_state = 0x02;
    ret = 0;
    break;
  }
  case VT_RELDISP:
  case VT_ACTIVATE:
  case VT_WAITACTIVE:
  case VT_DISALLOCATE:
    ret = 0;
    break;
  case KDSETMODE:
  case KDSKBMODE:
    ret = 0;
    break;
  case KDGETMODE: {
    int *mode = (int *)arg;
    if (!mode) {
      ret = (uint64_t)-14;
      break;
    }
    *mode = KD_TEXT;
    ret = 0;
    break;
  }
  case KDGKBMODE: {
    int *mode = (int *)arg;
    if (!mode) {
      ret = (uint64_t)-14;
      break;
    }
    *mode = 0;
    ret = 0;
    break;
  }
  case 0x5451:
    ret = 0;
    break; // KDSIGACCEPT
  case TIOCGETD: {
    int *ldisc = (int *)arg;
    if (!ldisc) {
      ret = (uint64_t)-14;
      break;
    }
    *ldisc = 0;
    ret = 0;
    break;
  }
  default:
    klog_puts("[IOCTL] ENOTTY fd=");
    klog_uint64(fd);
    klog_puts(" request=0x");
    klog_hex32((uint32_t)request);
    if (t && fd < MAX_FDS && t->fds[fd]) {
      klog_puts(" node=");
      klog_puts(t->fds[fd]->name);
    }
    klog_puts("\n");
    ret = (uint64_t)-25; // ENOTTY
    break;
  }

#if IOCTL_DEBUG_LOGGING
  klog_puts("[SYSCALL] sys_ioctl RETURN 0x");
  klog_hex64(ret);
  klog_puts("\n");
#endif
  return ret;
}

void syscall_register_ioctl(void) { syscall_register(SYS_IOCTL, sys_ioctl); }
