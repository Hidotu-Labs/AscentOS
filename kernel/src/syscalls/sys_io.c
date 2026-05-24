// ── I/O Syscalls: read, write, close, open, lseek ────────────────────────────
#include "../apic/lapic_timer.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../drivers/audio/sb16.h"
#include "../drivers/timer/pit.h"
#include "../fb/framebuffer.h"
#include "../font/font.h"
#include "../fs/procfs.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../net/net.h"
#include "../sched/sched.h"
#include "../sched/wait.h"
#include "../socket/af_unix.h"
#include "../socket/socket.h"
#include "syscall.h"
#include <stdint.h>

// User-space pointer validation: reject kernel/HHDM addresses
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL
static inline bool is_user_ptr(uint64_t addr) {
  return addr != 0 && addr <= USER_ADDR_MAX;
}

typedef struct {
  uint8_t *data;
  uint32_t capacity;
} ramfs_file_t;

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGWINSZ 0x5413

// VT (Virtual Terminal) ioctl constants
#define VT_OPENQRY 0x5600
#define VT_GETMODE 0x5601
#define VT_SETMODE 0x5602
#define VT_GETSTATE 0x5603
#define VT_RELDISP 0x5605
#define VT_ACTIVATE 0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608

// KD (Keyboard Display) ioctl constants
#define KDSETMODE 0x4B3A
#define KDGETMODE 0x4B3B
#define KDGKBMODE 0x4B44
#define KDSKBMODE 0x4B45
#define KD_TEXT 0x00
#define KD_GRAPHICS 0x01

struct vt_mode {
  char mode;
  char waitv;
  short relsig;
  short acqsig;
  short frsig;
};

struct vt_stat {
  unsigned short v_active;
  unsigned short v_signal;
  unsigned short v_state;
};

// OSS /dev/dsp ioctls
#define SNDCTL_DSP_RESET 0x00005000
#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_STEREO 0xC0045003
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006

#define AFMT_U8 0x00000008
#define AFMT_S16_LE 0x00000010

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_CLOEXEC 0x80000

// fcntl commands
#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_SETOWN 8
#define F_DUPFD_CLOEXEC 1030 // F_LINUX_SPECIFIC_BASE + 0

// File descriptor flags
#define FD_CLOEXEC 1

#define AT_FDCWD -100

// ── Linux x86_64 stat structure (matches musl struct stat)
// ────────────────────
struct kstat {
  uint64_t st_dev;   // Device
  uint64_t st_ino;   // Inode
  uint64_t st_nlink; // Number of hard links

  uint32_t st_mode;   // Mode (file type + permissions)
  uint32_t st_uid;    // User ID
  uint32_t st_gid;    // Group ID
  uint32_t __pad0;    // Padding
  uint64_t st_rdev;   // Device ID (if special file)
  int64_t st_size;    // Total size in bytes
  int64_t st_blksize; // Block size for filesystem I/O
  int64_t st_blocks;  // Number of 512B blocks allocated

  int64_t st_atim_sec;  // Access time seconds
  int64_t st_atim_nsec; // Access time nanoseconds
  int64_t st_mtim_sec;  // Modification time seconds
  int64_t st_mtim_nsec; // Modification time nanoseconds
  int64_t st_ctim_sec;  // Status change time seconds
  int64_t st_ctim_nsec; // Status change time nanoseconds
  int64_t __unused[3];  // Unused padding
};

struct statx_timestamp {
  int64_t tv_sec;
  uint32_t tv_nsec;
  int32_t __reserved;
};

struct statx {
  uint32_t stx_mask;
  uint32_t stx_blksize;
  uint64_t stx_attributes;
  uint32_t stx_nlink;
  uint32_t stx_uid;
  uint32_t stx_gid;
  uint16_t stx_mode;
  uint16_t __spare0[1];
  uint64_t stx_ino;
  uint64_t stx_size;
  uint64_t stx_blocks;
  uint64_t stx_attributes_mask;
  struct statx_timestamp stx_atime;
  struct statx_timestamp stx_btime;
  struct statx_timestamp stx_ctime;
  struct statx_timestamp stx_mtime;
  uint32_t stx_rdev_major;
  uint32_t stx_rdev_minor;
  uint32_t stx_dev_major;
  uint32_t stx_dev_minor;
  uint64_t __spare2[14];
};

#define STATX_TYPE 0x0001U
#define STATX_MODE 0x0002U
#define STATX_NLINK 0x0004U
#define STATX_UID 0x0008U
#define STATX_GID 0x0010U
#define STATX_ATIME 0x0020U
#define STATX_MTIME 0x0040U
#define STATX_CTIME 0x0080U
#define STATX_INO 0x0100U
#define STATX_SIZE 0x0200U
#define STATX_BLOCKS 0x0400U
#define STATX_BASIC_STATS 0x07ffU
#define STATX_BTIME 0x0800U

#define AT_STATX_SYNC_AS_STAT 0x0000
#define AT_STATX_FORCE_SYNC 0x2000
#define AT_STATX_DONT_SYNC 0x4000
#define AT_STATX_SYNC_TYPE 0x6000
#define AT_EMPTY_PATH 0x1000

#include "../drivers/pty.h"
#include "../fb/terminal.h"

struct termios console_termios;

// ── inotify implementation ──────────────────────────────────────────────────
// Simple inotify support for file/directory monitoring
#define MAX_INOTIFY_WATCHES 128
#define MAX_INOTIFY_INSTANCES 32

typedef struct {
  uint32_t wd;        // Watch descriptor
  char path[256];     // Path being watched
  uint32_t mask;      // Event mask (IN_*)
  uint32_t event_mask;// Accumulated events
} inotify_watch_t;

typedef struct {
  uint32_t instance_id;  // Unique inotify instance ID
  inotify_watch_t watches[MAX_INOTIFY_WATCHES];
  uint32_t num_watches;
  uint64_t event_queue[256]; // Simple event queue
  uint32_t queue_head;
  uint32_t queue_tail;
} inotify_instance_t;

// Global inotify instances
static inotify_instance_t inotify_instances[MAX_INOTIFY_INSTANCES];
static uint32_t next_instance_id = 1;
static uint32_t next_watch_id = 1;

// inotify events (basic subset)
#define IN_ACCESS 0x1
#define IN_MODIFY 0x2
#define IN_ATTRIB 0x4
#define IN_CLOSE_WRITE 0x8
#define IN_CLOSE_NOWRITE 0x10
#define IN_OPEN 0x20
#define IN_MOVED_FROM 0x40
#define IN_MOVED_TO 0x80
#define IN_CREATE 0x100
#define IN_DELETE 0x200
#define IN_DELETE_SELF 0x400
#define IN_MOVE_SELF 0x800

int alloc_fd(struct thread *t) {
  klog_puts("[SYSCALL] alloc_fd: t=");
  klog_uint64((uint64_t)t);
  klog_puts(" tid=");
  klog_uint64(t->tid);
  klog_puts(" fds_array=");
  klog_uint64((uint64_t)&t->fds[0]);
  klog_puts(" fds[4]_addr=");
  klog_uint64((uint64_t)&t->fds[4]);
  klog_puts("\n");
  for (int i = 0; i < MAX_FDS; i++) {
    if (t->fds[i] == NULL) {
      klog_puts("[SYSCALL] alloc_fd: found empty fd=");
      klog_uint64(i);
      klog_puts(" fds[4]=");
      klog_uint64((uint64_t)t->fds[4]);
      klog_puts("\n");
      return i;
    }
  }
  return -1; // ENFILE
}

static uint64_t do_sys_open(int dirfd, const char *path, uint64_t flags,
                            uint64_t mode) {
  (void)dirfd;
  if (!path)
    return (uint64_t)-14; // EFAULT

  (void)flags;
  (void)mode;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  int fd = alloc_fd(t);
  if (fd < 0)
    return (uint64_t)-24; // EMFILE

  klog_puts("[SYSCALL] open: path=");
  klog_puts(path);
  klog_puts(" fd=");
  klog_uint64(fd);
  klog_puts(" tid=");
  klog_uint64(t->tid);
  klog_puts("\n");

  vfs_node_t *base_dir = fs_root;
  if (path[0] != '/') {
    if (dirfd == AT_FDCWD) {
      base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
      if (!base_dir)
        base_dir = fs_root; // Fallback
    } else {
      if (dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
        return (uint64_t)-9; // EBADF
      base_dir = t->fds[dirfd];
      if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    }
  }

  // Check if this is a device file path (/dev/...)
  vfs_node_t *node = NULL;
  const char *dev_path = NULL;
  if (strncmp(path, "/dev/", 5) == 0)
    dev_path = path + 5;
  else if (strncmp(path, "dev/", 4) == 0)
    dev_path = path + 4;

  if (dev_path) {
    // Special handling for /dev/ptmx - each open creates a new PTY pair
    if (strcmp(dev_path, "ptmx") == 0) {
      int pty_index = pty_alloc_pair();
      if (pty_index < 0) {
        return (uint64_t)-16; // EBUSY
      }

      pty_pair_t *pty = pty_get_pair(pty_index);
      if (!pty) {
        return (uint64_t)-16;
      }

      // Create a unique VFS node for this PTY master
      node = kmalloc(sizeof(vfs_node_t));
      if (!node) {
        return (uint64_t)-12; // ENOMEM
      }
      vfs_node_init(node);
      strcpy(node->name, "ptmx");
      node->flags = FS_CHARDEV;
      node->mask = 0666;
      node->read = ptmx_read;
      node->write = ptmx_write;
      node->ioctl = ptmx_ioctl;
      node->poll = ptmx_poll;
      node->close = ptmx_close;
      node->mmap = ptmx_mmap;
      node->device = pty;
      node->wait_queue = pty->master_waitq;

      goto open_done;
    }

    // Special handling for /dev/pts/N - PTY slave devices
    if (strncmp(dev_path, "pts/", 4) == 0) {
      const char *num_str = dev_path + 4;
      int pty_index = 0;
      while (*num_str >= '0' && *num_str <= '9') {
        pty_index = pty_index * 10 + (*num_str - '0');
        num_str++;
      }

      if (*num_str != '\0') {
        return (uint64_t)-2; // ENOENT - invalid path
      }

      pty_pair_t *pty = pty_get_pair(pty_index);
      if (!pty || pty->locked) {
        return (uint64_t)-2; // ENOENT or EACCES
      }

      // Create a unique VFS node for this PTY slave
      node = kmalloc(sizeof(vfs_node_t));
      if (!node) {
        return (uint64_t)-12; // ENOMEM
      }
      vfs_node_init(node);
      strcpy(node->name, dev_path);
      node->flags = FS_CHARDEV;
      node->mask = 0620; // crw--w---- typical for PTY slaves
      node->read = pty_slave_read;
      node->write = pty_slave_write;
      node->ioctl = pty_slave_ioctl;
      node->poll = pty_slave_poll;
      node->open = pty_slave_open;
      node->close = pty_slave_close;
      node->mmap = pty_slave_mmap;
      node->device = pty;
      node->wait_queue = pty->slave_waitq;

      goto open_done;
    }

    // Special handling for /dev/tty - return controlling terminal
    if (strcmp(dev_path, "tty") == 0) {
      struct thread *t = sched_get_current();
      if (t && t->ctty) {
        // Return the controlling terminal (PTY slave)
        node = t->ctty;
        vfs_open(node); // Increment reference count
        klog_puts("[SYSCALL] /dev/tty -> ctty for tid=");
        klog_uint64(t->tid);
        klog_puts("\n");
        goto open_done;
      }
      // Fall through to console if no ctty set
    }

    // Try to get from device registry first
    node = fb_lookup_device((char *)dev_path);
  }

  // Fall back to normal VFS path resolution handles everything else
  if (!node) {
    node = vfs_resolve_path_at(base_dir, path);
  }

  if (!node) {
    if (flags & O_CREAT) {
      char parent_path[128];
      char file_name[128];
      size_t len = strlen(path);
      if (len == 0 || len >= sizeof(file_name))
        return (uint64_t)-14;

      const char *slash = 0;
      for (const char *p = path; *p; p++)
        if (*p == '/')
          slash = p;

      vfs_node_t *parent = base_dir;
      if (slash) {
        size_t parent_len = (size_t)(slash - path);
        if (parent_len == 0) {
          parent = fs_root;
        } else {
          if (parent_len >= sizeof(parent_path))
            return (uint64_t)-14;
          for (size_t i = 0; i < parent_len; i++)
            parent_path[i] = path[i];
          parent_path[parent_len] = '\0';
          parent = vfs_resolve_path_at(base_dir, parent_path);
        }
        size_t file_len = strlen(slash + 1);
        if (file_len >= sizeof(file_name))
          return (uint64_t)-14;
        strcpy(file_name, slash + 1);
      } else {
        size_t file_len = len;
        if (file_len >= sizeof(file_name))
          return (uint64_t)-14;
        strcpy(file_name, path);
      }

      if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR

      // Umask equivalent (simplified)
      mode = mode & ~0022; // Basic default umask

      if (vfs_create(parent, file_name, (uint16_t)mode) != 0)
        return (uint64_t)-17; // EEXIST or generic error

      node = vfs_finddir(parent, file_name);
      if (!node)
        return (uint64_t)-2; // ENOENT
    } else {
      return (uint64_t)-2; // ENOENT
    }
  }

  if ((flags & O_TRUNC) && node->flags == FS_FILE)
    node->length = 0;

open_done:
  klog_puts("[SYSCALL] open_done BEFORE: fd=");
  klog_uint64(fd);
  klog_puts(" fds[4]=");
  klog_uint64((uint64_t)t->fds[4]);
  klog_puts("\n");
  vfs_open(node);
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;
  
  // Store the full path for fchdir support
  if (path[0] == '/') {
    // Absolute path - use as-is
    strncpy(t->fd_paths[fd], path, sizeof(t->fd_paths[fd]) - 1);
    t->fd_paths[fd][sizeof(t->fd_paths[fd]) - 1] = '\0';
  } else {
    // Relative path - build full path from cwd
    t->fd_paths[fd][0] = '\0';
    if (t->cwd_path[0] && strcmp(t->cwd_path, "/") != 0) {
      strncpy(t->fd_paths[fd], t->cwd_path, sizeof(t->fd_paths[fd]) - 1);
      strncat(t->fd_paths[fd], "/", sizeof(t->fd_paths[fd]) - strlen(t->fd_paths[fd]) - 1);
      strncat(t->fd_paths[fd], path, sizeof(t->fd_paths[fd]) - strlen(t->fd_paths[fd]) - 1);
    } else {
      strcpy(t->fd_paths[fd], "/");
      strncat(t->fd_paths[fd], path, sizeof(t->fd_paths[fd]) - 2);
    }
    t->fd_paths[fd][sizeof(t->fd_paths[fd]) - 1] = '\0';
  }
  
  klog_puts("[SYSCALL] open_done AFTER: fd=");
  klog_uint64(fd);
  klog_puts(" node=");
  klog_uint64((uint64_t)node);
  klog_puts(" fds[fd]=");
  klog_uint64((uint64_t)t->fds[fd]);
  klog_puts(" fds[4]=");
  klog_uint64((uint64_t)t->fds[4]);
  klog_puts("\n");

  return fd;
}

static uint64_t sys_openat(uint64_t dirfd, uint64_t path_ptr, uint64_t flags,
                           uint64_t mode, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  return (uint64_t)(int)do_sys_open((int)dirfd, (const char *)path_ptr, flags,
                                    mode);
}

static uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return sys_openat(AT_FDCWD, path_ptr, flags, mode, 0, 0);
}

static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  klog_puts("[SYSCALL] dup2: oldfd=");
  klog_uint64(oldfd);
  klog_puts(" newfd=");
  klog_uint64(newfd);
  klog_puts("\n");
  struct thread *t = sched_get_current();
  if (!t || oldfd >= MAX_FDS || newfd >= MAX_FDS || !t->fds[oldfd])
    return (uint64_t)-9;

  t->fds[newfd] = t->fds[oldfd];
  t->fd_offsets[newfd] = t->fd_offsets[oldfd];
  vfs_open(t->fds[newfd]);

  return newfd;
}

static uint64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  klog_puts("[SYSCALL] close: fd=");
  klog_uint64(fd);
  klog_puts(" tid=");
  klog_uint64(t->tid);
  klog_puts("\n");
  vfs_close(t->fds[fd]);
  t->fds[fd] = NULL;
  return 0;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  klog_puts("[SYSCALL] read ENTER tid=");
  klog_uint64(t->tid);
  klog_puts(" fd=");
  klog_uint64(fd);
  klog_puts(" count=");
  klog_uint64(count);
  klog_puts(" fds[0]=");
  klog_uint64((uint64_t)(t ? t->fds[0] : 0));
  klog_puts("\n");

  if (!is_user_ptr(buf) || !vmm_is_user_addr_range_valid(buf, count))
    return (uint64_t)-14; // EFAULT

  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  int32_t bytes_read =
      (int32_t)vfs_read(node, t->fd_offsets[fd], count, (uint8_t *)buf);

  if (bytes_read > 0) {
    t->fd_offsets[fd] += (uint32_t)bytes_read;
  }

  return (uint64_t)(int64_t)bytes_read;
}

static uint64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE)
    return (uint64_t)-1; // EPERM

  if (vfs_truncate(node, (uint32_t)length) == 0) {
    return 0;
  }
  return (uint64_t)-1;
}

// ── fd_write ─────────────────────────────────────────────────────────────────
// For fd 0/1/2 (stdout / stderr) we use console_write_batch() so that each
// write() syscall results in exactly ONE framebuffer blit.  This kills the
// per-character flicker that kilo was triggering.
static int64_t fd_write(int fd, const void *buf, size_t count) {
  struct thread *t = sched_get_current();
  if (!t || fd < 0 || fd >= MAX_FDS || !t->fds[fd])
    return -9; // EBADF

  vfs_node_t *node = t->fds[fd];

  int32_t bytes_written =
      (int32_t)vfs_write(node, t->fd_offsets[fd], count, (uint8_t *)buf);

  if (bytes_written > 0) {
    t->fd_offsets[fd] += (uint32_t)bytes_written;
  }

  return (int64_t)bytes_written;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (!is_user_ptr(buf) || !vmm_is_user_addr_range_valid(buf, count))
    return (uint64_t)-14; // EFAULT

  /*
  {
    klog_puts("[SYSCALL] write tid=");
    klog_uint64(sched_get_current()->tid);
    klog_puts(" fd=");
    klog_uint64(fd);
    klog_puts(" count=");
    klog_uint64(count);
    klog_puts("\n");
  }
  */

  int64_t r = fd_write((int)fd, (const void *)buf, (size_t)count);

  return (uint64_t)r;
}

struct user_iovec {
  uint64_t iov_base;
  uint64_t iov_len;
};

static uint64_t sys_writev(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return (uint64_t)-22; // EINVAL

  struct user_iovec *iov = (struct user_iovec *)iov_u;

  struct thread *t = sched_get_current();
  if (t && t->tid == 13) {
    klog_puts("[SYSCALL] writev tid=13 fd=");
    klog_uint64(fd);
    klog_puts(" iovcnt=");
    klog_uint64(iovcnt);
    klog_puts("\n");
  }

  size_t total = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;
    int64_t w = fd_write((int)fd, (const void *)base, (size_t)len);
    if (w < 0) {
      if (t && t->tid == 13) {
        klog_puts("[SYSCALL] writev tid=13 RETURN ERROR=");
        klog_uint64((uint64_t)(-w));
        klog_puts("\n");
      }
      return (uint64_t)w;
    }
    total += (size_t)w;
    if ((size_t)w != len)
      break;
  }
  if (t && t->tid == 13) {
    klog_puts("[SYSCALL] writev tid=13 RETURN=");
    klog_uint64(total);
    klog_puts("\n");
  }
  return total;
}

static uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  /*
    klog_puts("[SYSCALL] sys_ioctl fd=");
    klog_uint64(fd);
    klog_puts(" request=");
    klog_hex64(request);
    klog_puts(" arg=");
    klog_hex64(arg);
    klog_puts("\n");
  */

  // Most ioctls take pointers. A few take ints. However, no valid integer
  // argument or user pointer should ever be in the kernel/HHDM address range.
  if (arg > USER_ADDR_MAX)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS)
    return (uint64_t)-9; // EBADF

  if (fd < MAX_FDS && t->fds[fd]) {
    vfs_node_t *node = t->fds[fd];
    if (node->ioctl) {
      // Generic validation for Linux-encoded ioctls (bits 31:30 determine R/W)
      // IOC_WRITE (0x40000000) -> kernel reads from user
      // IOC_READ (0x80000000) -> kernel writes to user
      if (request & 0xC0000000) {
        size_t sz = (request >> 16) & 0x3FFF;
        if (sz > 0 && !vmm_is_user_addr_range_valid(arg, sz)) {
          klog_puts(
              "[SYSCALL] ioctl: invalid arg pointer for encoded request\n");
          return (uint64_t)-14; // EFAULT
        }
      }

      uint64_t res = (uint64_t)node->ioctl(node, (uint32_t)request, arg);
      if (res != (uint64_t)-25) {
        return res;
      }
    }
  }

  switch ((uint32_t)request) {
  case TIOCGWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws || !vmm_is_user_addr_range_valid(arg, sizeof(struct winsize)))
      return (uint64_t)-14; // EFAULT
    ws->ws_row = (unsigned short)(fb_get_height() / FONT_HEIGHT);
    ws->ws_col = (unsigned short)(fb_get_width() / FONT_WIDTH);
    ws->ws_xpixel = (unsigned short)fb_get_width();
    ws->ws_ypixel = (unsigned short)fb_get_height();

    return 0;
  }
  case TCGETS: {
    struct termios *t = (struct termios *)arg;
    if (!t || !vmm_is_user_addr_range_valid(arg, sizeof(struct termios)))
      return -14;
    extern struct termios console_termios;
    *t = console_termios;
    return 0;
  }
  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    const struct termios *term = (const struct termios *)arg;
    if (!term || !vmm_is_user_addr_range_valid(arg, sizeof(struct termios)))
      return (uint64_t)-14;
    console_termios = *term;
    return 0;
  }
  case 0x5470: { // KBDSCANMODE_GET
    int *mode = (int *)arg;
    if (!mode)
      return (uint64_t)-14;
    extern bool keyboard_is_scancode_mode(void);
    *mode = keyboard_is_scancode_mode() ? 1 : 0;
    return 0;
  }
  case 0x5471: { // KBDSCANMODE_SET
    int mode = (int)arg;
    extern void keyboard_set_scancode_mode(bool enabled);
    keyboard_set_scancode_mode(mode != 0);
    return 0;
  }
  case 0x5472: { // KBDSCANCODE_READ
    // Scancode event structure: {scancode(1), is_extended(1), is_release(1)}
    unsigned char *event = (unsigned char *)arg;
    if (!event)
      return (uint64_t)-14;

    extern bool keyboard_has_scancode(void);
    extern bool keyboard_get_scancode(void *event_ptr);

    if (keyboard_has_scancode()) {
      if (keyboard_get_scancode((void *)event)) {
        return 1; // 1 scancode event read
      }
    }
    return 0; // No scancode available (would block in non-blocking mode)
  }
  // VT ioctl stubs for X11 server support
  case VT_OPENQRY: {
    int *vt = (int *)arg;
    if (!vt)
      return (uint64_t)-14;
    *vt = 1;
    return 0;
  }
  case VT_GETMODE: {
    struct vt_mode *vtm = (struct vt_mode *)arg;
    if (!vtm)
      return (uint64_t)-14;
    memset(vtm, 0, sizeof(struct vt_mode));
    return 0;
  }
  case VT_SETMODE:
    return 0;
  case VT_GETSTATE: {
    struct vt_stat *vts = (struct vt_stat *)arg;
    if (!vts)
      return (uint64_t)-14;
    memset(vts, 0, sizeof(struct vt_stat));
    vts->v_active = 1;
    vts->v_state = 0x02;
    return 0;
  }
  case VT_RELDISP:
  case VT_ACTIVATE:
  case VT_WAITACTIVE:
  case VT_DISALLOCATE:
    return 0;
  case KDSETMODE:
  case KDSKBMODE:
    return 0;
  case KDGETMODE: {
    int *mode = (int *)arg;
    if (!mode)
      return (uint64_t)-14;
    *mode = KD_TEXT;
    return 0;
  }
  case KDGKBMODE: {
    int *mode = (int *)arg;
    if (!mode)
      return (uint64_t)-14;
    *mode = 0; // K_XLATE
    return 0;
  }
  default:
    return (uint64_t)-25; // ENOTTY
  }
}

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];

  int64_t new_offset = 0;
  if (whence == 0) {
    new_offset = (int64_t)offset;
  } else if (whence == 1) {
    new_offset = (int64_t)t->fd_offsets[fd] + (int64_t)offset;
  } else if (whence == 2) {
    new_offset = (int64_t)node->length + (int64_t)offset;
  } else {
    return (uint64_t)-22; // EINVAL
  }

  if (new_offset < 0)
    return (uint64_t)-22;

  t->fd_offsets[fd] = (uint32_t)new_offset;
  return (uint64_t)new_offset;
}

static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  switch (cmd) {
  case F_DUPFD:
  case F_DUPFD_CLOEXEC: {
    // Duplicate fd to lowest available >= arg
    int newfd = alloc_fd(t);
    if (newfd < 0)
      return (uint64_t)-24; // EMFILE
    if ((uint64_t)newfd < arg) {
      // Find lowest available >= arg
      for (int i = (int)arg; i < MAX_FDS; i++) {
        if (t->fds[i] == NULL) {
          newfd = i;
          break;
        }
      }
      if ((uint64_t)newfd < arg)
        return (uint64_t)-24; // EMFILE
    }
    t->fds[newfd] = t->fds[fd];
    // For F_DUPFD_CLOEXEC, we'd set FD_CLOEXEC but we don't track per-FD flags
    // yet
    return (uint64_t)newfd;
  }
  case F_GETFD: {
    // Get file descriptor flags (we don't track per-FD flags yet, return 0)
    (void)arg;
    return 0;
  }
  case F_SETFD: {
    // Set file descriptor flags (FD_CLOEXEC)
    // We don't track per-FD flags yet, just succeed
    (void)arg;
    return 0;
  }
  case F_GETFL: {
    uint64_t flags = O_RDWR;
    vfs_node_t *node = t->fds[fd];
    if ((node->flags & FS_TYPE_MASK) == FS_SOCKET) {
      socket_t *sock = (socket_t *)node->device;
      if (sock && (sock->flags & SOCK_NONBLOCK)) {
        flags |= O_NONBLOCK;
      }
    }
    return flags;
  }
  case F_SETFL: {
    vfs_node_t *node = t->fds[fd];
    if ((node->flags & FS_TYPE_MASK) == FS_SOCKET) {
      socket_t *sock = (socket_t *)node->device;
      if (sock) {
        if (arg & O_NONBLOCK) {
          sock->flags |= SOCK_NONBLOCK;
        } else {
          sock->flags &= ~SOCK_NONBLOCK;
        }
      }
    }
    return 0;
  }
  case F_SETOWN: {
    // Set owner for SIGIO - not implemented, just succeed
    (void)arg;
    return 0;
  }
  default:
    return (uint64_t)-22; // EINVAL
  }
}

// ── Fill kstat from vfs_node ────────────────────────────────────────────────
static void fill_kstat(struct kstat *ks, vfs_node_t *node) {
  ks->st_dev = 0; // No device numbers yet
  ks->st_ino = (uint64_t)node->inode;
  ks->st_nlink = 1; // No hard link tracking yet

  // Convert vfs flags to mode
  uint32_t mode = node->mask & 0777; // Permission bits
  switch (node->flags & FS_TYPE_MASK) {
  case FS_FILE:
    mode |= 0100000;
    break; // S_IFREG
  case FS_DIRECTORY:
    mode |= 0040000;
    break; // S_IFDIR
  case FS_CHARDEV:
    mode |= 0020000;
    break; // S_IFCHR
  case FS_BLOCKDEV:
    mode |= 0060000;
    break; // S_IFBLK
  case FS_SYMLINK:
    mode |= 0120000;
    break; // S_IFLNK
  case FS_SOCKET:
    mode |= 0140000;
    break; // S_IFSOCK
  default:
    mode |= 0100000;
    break; // Default to regular file
  }
  ks->st_mode = mode;
  ks->st_uid = node->uid;
  ks->st_gid = node->gid;
  ks->__pad0 = 0;
  ks->st_rdev = 0;
  ks->st_size = (int64_t)node->length;
  ks->st_blksize = 4096; // Reasonable default
  ks->st_blocks = ((int64_t)node->length + 511) / 512;

  ks->st_atim_sec = (int64_t)node->atime;
  ks->st_atim_nsec = 0;
  ks->st_mtim_sec = (int64_t)node->mtime;
  ks->st_mtim_nsec = 0;
  ks->st_ctim_sec = (int64_t)node->ctime;
  ks->st_ctim_nsec = 0;
  ks->__unused[0] = 0;
  ks->__unused[1] = 0;
  ks->__unused[2] = 0;
}

// ── sys_stat: stat(path, statbuf) ────────────────────────────────────────────
static uint64_t sys_fadvise64(uint64_t fd, uint64_t offset, uint64_t len,
                              uint64_t advice, uint64_t a4, uint64_t a5) {
  (void)fd;
  (void)offset;
  (void)len;
  (void)advice;
  (void)a4;
  (void)a5;
  return 0;
}

static uint64_t sys_stat(uint64_t path_ptr, uint64_t statbuf_ptr, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)path_ptr;
  struct kstat *ks = (struct kstat *)statbuf_ptr;

  if (!path || !ks)
    return (uint64_t)-14; // EFAULT
  if (!is_user_ptr((uint64_t)ks))
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  vfs_node_t *node = NULL;

  // Check device registry for /dev/ paths
  if (strncmp(path, "/dev/", 5) == 0) {
    node = fb_lookup_device((char *)path + 5);
  }

  if (!node) {
    vfs_node_t *cwd_node = fs_root;
    if (t && t->cwd_path[0]) {
      cwd_node = vfs_resolve_path_at(fs_root, t->cwd_path);
      if (!cwd_node)
        cwd_node = fs_root;
    }
    node = vfs_resolve_path_at(cwd_node, path);
  }

  if (!node) {
    klog_puts("[SYSCALL] sys_stat: not found: ");
    klog_puts(path);
    klog_puts("\n");
    return (uint64_t)-2; // ENOENT
  }

  fill_kstat(ks, node);
  return 0;
}

// ── sys_lstat: lstat(path, statbuf) - like stat but doesn't follow symlinks
// For now we treat it the same as stat since we don't follow symlinks yet.
static uint64_t sys_lstat(uint64_t path_ptr, uint64_t statbuf_ptr, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  // For now, identical to stat (we don't follow symlinks)
  return sys_stat(path_ptr, statbuf_ptr, a2, a3, a4, a5);
}

// ── sys_fstat: fstat(fd, statbuf)
// ─────────────────────────────────────────────
static uint64_t sys_fstat(uint64_t fd, uint64_t statbuf_ptr, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct kstat *ks = (struct kstat *)statbuf_ptr;

  if (!ks)
    return (uint64_t)-14; // EFAULT
  if (!is_user_ptr((uint64_t)ks))
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  fill_kstat(ks, node);
  return 0;
}

// ── sys_statx: statx(dirfd, path, flags, mask, statxbuf) ─────────────────────
static uint64_t sys_statx(uint64_t dirfd, uint64_t path_ptr, uint64_t flags,
                          uint64_t mask, uint64_t statxbuf_ptr, uint64_t a5) {
  (void)a5;
  (void)mask;
  const char *path = (const char *)path_ptr;
  struct statx *stx = (struct statx *)statxbuf_ptr;

  if (!stx || !is_user_ptr((uint64_t)stx))
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  vfs_node_t *node = NULL;

  if (path && path[0] != '\0') {
    vfs_node_t *base_dir = fs_root;
    if (path[0] != '/') {
      if ((int)dirfd == AT_FDCWD) {
        base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base_dir)
          base_dir = fs_root;
      } else {
        if ((int)dirfd < 0 || (int)dirfd >= MAX_FDS || !t->fds[dirfd])
          return (uint64_t)-9; // EBADF
        base_dir = t->fds[dirfd];
      }
    }
    node = vfs_resolve_path_at(base_dir, path);
  } else {
    // pathname is NULL or empty string
    if (flags & AT_EMPTY_PATH) {
      if ((int)dirfd == AT_FDCWD) {
        node = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!node)
          node = fs_root;
      } else {
        if ((int)dirfd < 0 || (int)dirfd >= MAX_FDS || !t->fds[dirfd])
          return (uint64_t)-9; // EBADF
        node = t->fds[dirfd];
      }
    } else {
      return (uint64_t)-14; // EFAULT
    }
  }

  if (!node)
    return (uint64_t)-2; // ENOENT

  // Zero the whole structure first
  memset(stx, 0, sizeof(struct statx));

  // Populate statx
  stx->stx_mask = STATX_BASIC_STATS; // We support most basic attributes
  stx->stx_blksize = 4096;
  stx->stx_nlink = 1;
  stx->stx_uid = node->uid;
  stx->stx_gid = node->gid;

  uint32_t mode = node->mask & 0777;
  switch (node->flags & FS_TYPE_MASK) {
  case FS_FILE:
    mode |= 0100000;
    break;
  case FS_DIRECTORY:
    mode |= 0040000;
    break;
  case FS_CHARDEV:
    mode |= 0020000;
    break;
  case FS_BLOCKDEV:
    mode |= 0060000;
    break;
  case FS_SYMLINK:
    mode |= 0120000;
    break;
  default:
    mode |= 0100000;
    break;
  }
  stx->stx_mode = (uint16_t)mode;
  stx->stx_ino = node->inode;
  stx->stx_size = node->length;
  stx->stx_blocks = (node->length + 511) / 512;

  stx->stx_atime.tv_sec = (int64_t)node->atime;
  stx->stx_mtime.tv_sec = (int64_t)node->mtime;
  stx->stx_ctime.tv_sec = (int64_t)node->ctime;
  // stx_btime not supported by VFS, left as 0

  return 0;
}

// Linux getdents64 structure (binary compatible with musl/glibc)
struct linux_dirent64 {
  uint64_t d_ino;    // Inode number
  uint64_t d_off;    // Offset to next entry
  uint16_t d_reclen; // Size of this dirent
  uint8_t d_type;    // File type (DT_REG, DT_DIR, etc.)
  char d_name[];     // Filename (null-terminated)
};

// DT_* constants from Linux dirent.h
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

// getdents64(fd, dirp, count) - read directory entries
static uint64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if ((node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  uint8_t *buf = (uint8_t *)dirp;
  if (!buf)
    return (uint64_t)-14; // EFAULT
  if (!is_user_ptr((uint64_t)buf))
    return (uint64_t)-14; // EFAULT

  size_t written = 0;
  uint32_t index = t->fd_offsets[fd]; // Start from saved position

  while (1) {
    struct dirent *de = vfs_readdir(node, index);
    if (!de)
      break; // No more entries

    size_t name_len = strlen(de->name);
    size_t entry_size = sizeof(struct linux_dirent64) + name_len + 1;
    entry_size = (entry_size + 7) & ~7; // Align to 8 bytes

    if (written + entry_size > count) {
      // Buffer full - return what we have, don't advance index
      break;
    }

    struct linux_dirent64 *entry = (struct linux_dirent64 *)(buf + written);
    entry->d_ino = de->ino;
    entry->d_off = (uint64_t)(index + 1);
    entry->d_reclen = (uint16_t)entry_size;

    // Determine file type from VFS node
    vfs_node_t *child = vfs_finddir(node, de->name);
    if (child) {
      switch (child->flags & FS_TYPE_MASK) {
      case FS_FILE:
        entry->d_type = DT_REG;
        break;
      case FS_DIRECTORY:
        entry->d_type = DT_DIR;
        break;
      case FS_CHARDEV:
        entry->d_type = DT_CHR;
        break;
      case FS_BLOCKDEV:
        entry->d_type = DT_BLK;
        break;
      case FS_SYMLINK:
        entry->d_type = DT_LNK;
        break;
      default:
        entry->d_type = DT_UNKNOWN;
        break;
      }
    } else {
      entry->d_type = DT_UNKNOWN;
    }

    strcpy(entry->d_name, de->name);
    written += entry_size;
    index++;
  }

  // Update saved position for next call
  t->fd_offsets[fd] = index;

  return written;
}

struct linux_dirent {
  uint64_t d_ino;
  uint64_t d_off;
  uint16_t d_reclen;
  char d_name[];
};

static uint64_t sys_getdents(uint64_t fd, uint64_t dirp, uint64_t count,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;
  vfs_node_t *node = t->fds[fd];
  if ((node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return (uint64_t)-20;
  uint8_t *buf = (uint8_t *)dirp;
  if (!buf || !vmm_is_user_addr_range_valid(dirp, count))
    return (uint64_t)-14;
  size_t written = 0;
  uint32_t index = t->fd_offsets[fd];
  while (1) {
    struct dirent *de = vfs_readdir(node, index);
    if (!de)
      break;
    size_t name_len = strlen(de->name);
    size_t entry_size = 8 + 8 + 2 + name_len + 2;
    entry_size = (entry_size + 7) & ~7;
    if (written + entry_size > count)
      break;
    struct linux_dirent *entry = (struct linux_dirent *)(buf + written);
    entry->d_ino = de->ino;
    entry->d_off = (uint64_t)(index + 1);
    entry->d_reclen = (uint16_t)entry_size;
    strcpy(entry->d_name, de->name);
    buf[written + entry_size - 1] = 0; // DT_UNKNOWN
    written += entry_size;
    index++;
  }
  t->fd_offsets[fd] = index;
  return written;
}

// Simple xorshift64 PRNG state
static uint64_t prng_state = 0;

static void prng_seed(void) {
  if (prng_state != 0)
    return;

  // Use RDTSC for initial entropy
  uint64_t tsc;
  __asm__ volatile("rdtsc" : "=A"(tsc));

  // Mix with PIT ticks if available
  uint64_t ticks = lapic_timer_get_ticks();

  // Combine sources
  prng_state = tsc ^ (ticks << 32) ^ 0xDEADBEEFCAFEBABEULL;

  // Ensure non-zero
  if (prng_state == 0)
    prng_state = 1;
}

static uint64_t xorshift64(void) {
  uint64_t x = prng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  prng_state = x;
  return x;
}

// getrandom(2) - syscall 318
static uint64_t sys_getrandom(uint64_t buf_ptr, uint64_t buflen, uint64_t flags,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!buf_ptr)
    return (uint64_t)-14; // EFAULT
  if (!is_user_ptr(buf_ptr))
    return (uint64_t)-14; // EFAULT
  if (buflen == 0)
    return 0;

  // Initialize PRNG if needed
  prng_seed();

  uint8_t *buf = (uint8_t *)buf_ptr;
  uint64_t written = 0;

  // Fill buffer with random bytes
  while (written < buflen) {
    uint64_t rand_val = xorshift64();

    // Write up to 8 bytes at a time
    for (int i = 0; i < 8 && written < buflen; i++) {
      buf[written++] = (uint8_t)(rand_val & 0xFF);
      rand_val >>= 8;
    }
  }

  return written;
}

// ── sys_mkdir: mkdir(pathname, mode) — syscall 83 ──────────────────────────
static uint64_t sys_mkdir(uint64_t pathname, uint64_t mode, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname;
  if (!path)
    return (uint64_t)-14; // EFAULT

  klog_puts("[MKDIR] path=");
  klog_puts(path);
  klog_puts(" mode=");
  klog_uint64(mode);
  klog_puts("\n");

  // Split path into parent directory + new dir name
  char parent_path[256];
  char dir_name[128];
  size_t len = strlen(path);
  if (len == 0 || len >= 256)
    return (uint64_t)-14;

  const char *slash = 0;
  for (const char *p = path; *p; p++)
    if (*p == '/')
      slash = p;

  vfs_node_t *parent = fs_root;
  if (slash) {
    size_t parent_len = (size_t)(slash - path);
    if (parent_len == 0) {
      parent = fs_root;
    } else {
      if (parent_len >= sizeof(parent_path))
        return (uint64_t)-14;
      for (size_t i = 0; i < parent_len; i++)
        parent_path[i] = path[i];
      parent_path[parent_len] = '\0';
      parent = vfs_resolve_path(parent_path);
      
      // Parent yoksa recursive mkdir yap
      if (!parent) {
        klog_puts("[MKDIR] Parent not found, creating recursively\n");
        sys_mkdir((uint64_t)parent_path, mode, 0, 0, 0, 0);
        parent = vfs_resolve_path(parent_path);
      }
    }
    size_t dlen = strlen(slash + 1);
    if (dlen == 0 || dlen >= sizeof(dir_name))
      return (uint64_t)-22; // EINVAL
    strcpy(dir_name, slash + 1);
  } else {
    strcpy(dir_name, path);
  }

  if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  if (vfs_mkdir(parent, dir_name, (uint16_t)mode) != 0)
    return (uint64_t)-17; // EEXIST

  return 0;
}

// ── sys_mkdirat: mkdirat(dirfd, pathname, mode) — syscall 258 ────────────────
static uint64_t sys_mkdirat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname;
  if (!path)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  // Resolve base directory from dirfd
  vfs_node_t *base_dir = fs_root;
  if (path[0] != '/') {
    if ((int64_t)dirfd == AT_FDCWD) {
      base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
      if (!base_dir)
        base_dir = fs_root;
    } else if (dirfd < MAX_FDS && t->fds[dirfd]) {
      base_dir = t->fds[dirfd];
      if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    } else {
      return (uint64_t)-9; // EBADF
    }
  }

  // Split path into parent directory + new dir name
  char parent_path[128];
  char dir_name[128];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(dir_name))
    return (uint64_t)-14;

  const char *slash = 0;
  for (const char *p = path; *p; p++)
    if (*p == '/')
      slash = p;

  vfs_node_t *parent = base_dir;
  if (slash) {
    size_t parent_len = (size_t)(slash - path);
    if (parent_len == 0) {
      parent = fs_root;
    } else {
      if (parent_len >= sizeof(parent_path))
        return (uint64_t)-14;
      for (size_t i = 0; i < parent_len; i++)
        parent_path[i] = path[i];
      parent_path[parent_len] = '\0';
      parent = vfs_resolve_path_at(base_dir, parent_path);
    }
    size_t dlen = strlen(slash + 1);
    if (dlen == 0 || dlen >= sizeof(dir_name))
      return (uint64_t)-22; // EINVAL
    strcpy(dir_name, slash + 1);
  } else {
    strcpy(dir_name, path);
  }

  if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  if (vfs_mkdir(parent, dir_name, (uint16_t)mode) != 0)
    return (uint64_t)-17; // EEXIST

  return 0;
}

// ── sys_unlinkat: unlinkat(dirfd, pathname, flags) — syscall 263 ────────────
static uint64_t sys_unlinkat(uint64_t dirfd, uint64_t pathname, uint64_t flags,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname;
  if (!path)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  // Resolve base directory from dirfd
  vfs_node_t *base_dir = fs_root;
  if (path[0] != '/') {
    if ((int64_t)dirfd == AT_FDCWD) {
      base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
      if (!base_dir)
        base_dir = fs_root;
    } else if (dirfd < MAX_FDS && t->fds[dirfd]) {
      base_dir = t->fds[dirfd];
      if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    } else {
      return (uint64_t)-9; // EBADF
    }
  }

  // Split path into parent directory + file name
  char parent_path[128];
  char file_name[128];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(file_name))
    return (uint64_t)-14;

  const char *slash = 0;
  for (const char *p = path; *p; p++)
    if (*p == '/')
      slash = p;

  vfs_node_t *parent = base_dir;
  if (slash) {
    size_t parent_len = (size_t)(slash - path);
    if (parent_len == 0) {
      parent = fs_root;
    } else {
      if (parent_len >= sizeof(parent_path))
        return (uint64_t)-14;
      for (size_t i = 0; i < parent_len; i++)
        parent_path[i] = path[i];
      parent_path[parent_len] = '\0';
      parent = vfs_resolve_path_at(base_dir, parent_path);
    }
    size_t flen = strlen(slash + 1);
    if (flen == 0 || flen >= sizeof(file_name))
      return (uint64_t)-22; // EINVAL
    strcpy(file_name, slash + 1);
  } else {
    strcpy(file_name, path);
  }

  if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  // Build full path for socket unbinding
  char full_path[256];
  if (path[0] == '/') {
    // Absolute path
    strncpy(full_path, path, sizeof(full_path) - 1);
    full_path[sizeof(full_path) - 1] = '\0';
  } else {
    // Relative path - construct from cwd
    if (t->cwd_path[0]) {
      strncpy(full_path, t->cwd_path, sizeof(full_path) - 1);
      full_path[sizeof(full_path) - 1] = '\0';
      strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
      strncat(full_path, path, sizeof(full_path) - strlen(full_path) - 1);
    } else {
      strncpy(full_path, "/", sizeof(full_path) - 1);
      strncat(full_path, path, sizeof(full_path) - strlen(full_path) - 1);
    }
  }

  // Unbind socket from internal list before filesystem unlink
  unix_unbind_by_path(full_path);

  if (vfs_unlink(parent, file_name) != 0)
    return (uint64_t)-2; // ENOENT

  return 0;
}

// ── sys_readv: readv(fd, iov, iovcnt) — syscall 19 ──────────────────────────
static uint64_t sys_readv(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  klog_puts("[SYSCALL] readv ENTER tid=");
  klog_uint64(t->tid);
  klog_puts(" fd=");
  klog_uint64(fd);
  klog_puts(" iovcnt=");
  klog_uint64(iovcnt);
  klog_puts("\n");

  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return (uint64_t)-22; // EINVAL

  struct user_iovec *iov = (struct user_iovec *)iov_u;
  size_t total = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;
    if (!is_user_ptr(base))
      return (uint64_t)-14; // EFAULT

    // Reuse sys_read logic
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd])
      return (uint64_t)-9; // EBADF

    vfs_node_t *node = t->fds[fd];
    int32_t bytes_read =
        (int32_t)vfs_read(node, t->fd_offsets[fd], len, (uint8_t *)base);

    if (bytes_read < 0) {
      if (total > 0)
        break;
      return (uint64_t)(int64_t)bytes_read;
    }

    if (bytes_read > 0) {
      t->fd_offsets[fd] += (uint32_t)bytes_read;
    }

    total += (size_t)bytes_read;
    if ((uint32_t)bytes_read < len)
      break; // Short read
  }
  return total;
}

// ── sys_eventfd2: eventfd2(initval, flags) — syscall 290 ─────────────────────
#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC 02000000
#define EFD_NONBLOCK 04000

typedef struct {
  uint64_t counter;
  uint32_t flags;
  wait_queue_t wq;
  spinlock_t lock;
} eventfd_ctx_t;

static uint32_t eventfd_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
  (void)offset;
  if (size < 8)
    return (uint32_t)-22; // EINVAL
  eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->device;
  if (!ctx)
    return (uint32_t)-22;

  spinlock_acquire(&ctx->lock);
  if (ctx->counter == 0) {
    spinlock_release(&ctx->lock);
    return (uint32_t)-11; // EAGAIN
  }
  uint64_t val;
  if (ctx->flags & EFD_SEMAPHORE) {
    val = 1;
    ctx->counter -= 1;
  } else {
    val = ctx->counter;
    ctx->counter = 0;
  }
  spinlock_release(&ctx->lock);

  memcpy(buffer, &val, 8);
  return 8;
}

static uint32_t eventfd_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                              uint8_t *buffer) {
  (void)offset;
  if (size < 8)
    return (uint32_t)-22; // EINVAL
  eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->device;
  if (!ctx)
    return (uint32_t)-22;

  uint64_t val;
  memcpy(&val, buffer, 8);
  if (val == 0)
    return (uint32_t)-22;

  spinlock_acquire(&ctx->lock);
  ctx->counter += val;
  spinlock_release(&ctx->lock);

  wait_queue_wake_all(&ctx->wq);
  return 8;
}

static int eventfd_poll(vfs_node_t *node, int events) {
  eventfd_ctx_t *ctx = (eventfd_ctx_t *)node->device;
  if (!ctx)
    return 0;

  int revents = 0;
  spinlock_acquire(&ctx->lock);
  if (ctx->counter > 0)
    revents |= POLLIN;
  if (ctx->counter < 0xfffffffffffffffeULL)
    revents |= POLLOUT;
  spinlock_release(&ctx->lock);
  return revents & events;
}

static void eventfd_close(vfs_node_t *node) {
  if (node && node->device) {
    kfree(node->device);
  }
  kfree(node);
}

static uint64_t sys_eventfd2(uint64_t initval, uint64_t flags, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  int fd = alloc_fd(t);
  if (fd < 0)
    return (uint64_t)-24; // EMFILE

  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return (uint64_t)-12; // ENOMEM
  memset(node, 0, sizeof(vfs_node_t));

  eventfd_ctx_t *ctx = kmalloc(sizeof(eventfd_ctx_t));
  if (!ctx) {
    kfree(node);
    return (uint64_t)-12; // ENOMEM
  }
  memset(ctx, 0, sizeof(eventfd_ctx_t));
  ctx->counter = initval;
  ctx->flags = flags;
  spinlock_init(&ctx->lock);
  wait_queue_init(&ctx->wq);

  node->flags = FS_FILE;
  node->device = ctx;
  node->read = eventfd_read;
  node->write = eventfd_write;
  node->poll = eventfd_poll;
  node->close = eventfd_close;
  node->wait_queue = &ctx->wq; // Link wait queue for epoll/poll

  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;

  return fd;
}

static void pipe_close(vfs_node_t *node) {
  if (!node || !node->device)
    return;
  ramfs_file_t *file = (ramfs_file_t *)node->device;
  if (file->data) {
    kfree(file->data);
    file->data = NULL;
  }
  kfree(file);
  node->device = NULL;
}

// ── sys_pipe2: pipe2(pipefd, flags) — syscall 293 ────────────────────────────
static uint64_t sys_pipe2(uint64_t pipefd_ptr, uint64_t flags, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  int *pipefd = (int *)pipefd_ptr;
  if (!pipefd)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  // Allocate two file descriptors
  int fd_read = alloc_fd(t);
  if (fd_read < 0)
    return (uint64_t)-24;       // EMFILE
  t->fds[fd_read] = (void *)-1; // Reserve it

  int fd_write = alloc_fd(t);
  if (fd_write < 0) {
    t->fds[fd_read] = NULL;
    return (uint64_t)-24; // EMFILE
  }

  // Build pipe buffer node directly in kernel heap (avoids ext2 create issues)
  vfs_node_t *pipe_node = kmalloc(sizeof(vfs_node_t));
  if (!pipe_node) {
    t->fds[fd_read] = NULL;
    return (uint64_t)-12; // ENOMEM
  }
  vfs_node_init(pipe_node);

  ramfs_file_t *pipe_buf = kmalloc(sizeof(ramfs_file_t));
  if (!pipe_buf) {
    kfree(pipe_node);
    t->fds[fd_read] = NULL;
    return (uint64_t)-12;
  }
  pipe_buf->data = NULL;
  pipe_buf->capacity = 0;

  pipe_node->flags = FS_FILE;
  pipe_node->device = pipe_buf;
  pipe_node->length = 0;
  pipe_node->read = ramfs_read;
  pipe_node->write = ramfs_write;
  pipe_node->close = pipe_close;

  // Both fds point to the same node; read offset and write offset tracked
  // separately
  vfs_open(pipe_node);
  vfs_open(pipe_node);
  t->fds[fd_read] = pipe_node;
  t->fds[fd_write] = pipe_node;
  t->fd_offsets[fd_read] = 0;
  t->fd_offsets[fd_write] = 0;

  pipefd[0] = fd_read;
  pipefd[1] = fd_write;

  return 0;
}

// ── sys_pipe: pipe(pipefd) — syscall 22 ──────────────────────────────────────
static uint64_t sys_pipe(uint64_t pipefd_ptr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  return sys_pipe2(pipefd_ptr, 0, a2, a3, a4, a5);
}

// ── sys_access: access(pathname, mode) — syscall 21 ──────────────────────────
static uint64_t do_sys_access(int dirfd, const char *path, uint64_t mode,
                              int flags) {
  (void)flags;
  if (!path)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  vfs_node_t *node = NULL;

  if (path[0] == '/') {
    node = vfs_resolve_path_at(fs_root, path);
  } else if (strncmp(path, "/dev/", 5) == 0) {
    node = fb_lookup_device((char *)path + 5);
  }

  if (!node) {
    vfs_node_t *base_dir = fs_root;
    if (dirfd == AT_FDCWD) {
      if (t && t->cwd_path[0]) {
        base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base_dir)
          base_dir = fs_root;
      }
    } else {
      if (dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
        return (uint64_t)-9; // EBADF
      base_dir = t->fds[dirfd];
      if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    }
    node = vfs_resolve_path_at(base_dir, path);
  }

  if (!node) {
    return (uint64_t)-2; // ENOENT
  }

  // POSIX mode check.
  // mode bits: F_OK = 0, X_OK = 1, W_OK = 2, R_OK = 4.
  if (mode != 0) {
    bool can_read = (node->mask & 0444) != 0;
    bool can_write = (node->mask & 0222) != 0;
    bool can_exec = (node->mask & 0111) != 0;

    if ((mode & 4) && !can_read)
      return (uint64_t)-13; // EACCES
    if ((mode & 2) && !can_write)
      return (uint64_t)-13; // EACCES
    if ((mode & 1) && !can_exec)
      return (uint64_t)-13; // EACCES
  }

  return 0;
}

static uint64_t sys_access(uint64_t pathname_ptr, uint64_t mode, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return do_sys_access(AT_FDCWD, (const char *)pathname_ptr, mode, 0);
}

static uint64_t sys_faccessat2(uint64_t dirfd, uint64_t pathname_ptr,
                               uint64_t mode, uint64_t flags, uint64_t a4,
                               uint64_t a5) {
  (void)a4;
  (void)a5;
  return do_sys_access((int)dirfd, (const char *)pathname_ptr, mode,
                       (int)flags);
}

static uint64_t sys_fchmodat(uint64_t dirfd, uint64_t pathname_ptr,
                             uint64_t mode, uint64_t flags, uint64_t a4,
                             uint64_t a5) {
  (void)flags; // flags like AT_SYMLINK_NOFOLLOW largely ignored for now
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  if (!path)
    return (uint64_t)-14;

  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  if (path[0] != '/') {
    if ((int)dirfd == AT_FDCWD) {
      if (t && t->cwd_path[0]) {
        base = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base)
          base = fs_root;
      }
    } else {
      if (dirfd >= MAX_FDS || !t->fds[dirfd])
        return (uint64_t)-9;
      base = t->fds[dirfd];
    }
  }

  vfs_node_t *node = vfs_resolve_path_at(base, path);
  if (!node)
    return (uint64_t)-2;

  int ret = vfs_chmod(node, (uint16_t)mode);
  return ret == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_fchownat(uint64_t dirfd, uint64_t pathname_ptr,
                             uint64_t owner, uint64_t group, uint64_t flags,
                             uint64_t a5) {
  (void)flags;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  if (!path)
    return (uint64_t)-14;

  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  if (path[0] != '/') {
    if ((int)dirfd == AT_FDCWD) {
      if (t && t->cwd_path[0]) {
        base = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base)
          base = fs_root;
      }
    } else {
      if (dirfd >= MAX_FDS || !t->fds[dirfd])
        return (uint64_t)-9;
      base = t->fds[dirfd];
    }
  }

  vfs_node_t *node = vfs_resolve_path_at(base, path);
  if (!node)
    return (uint64_t)-2;

  uint32_t uid = (owner == (uint64_t)-1) ? node->uid : (uint32_t)owner;
  uint32_t gid = (group == (uint64_t)-1) ? node->gid : (uint32_t)group;

  int ret = vfs_chown(node, uid, gid);
  return ret == 0 ? 0 : (uint64_t)-1;
}

// ── sys_newfstatat: fstatat(dirfd, pathname, statbuf, flags) — syscall 262 ──
#define AT_EMPTY_PATH 0x1000

static uint64_t sys_newfstatat(uint64_t dirfd, uint64_t pathname_ptr,
                               uint64_t statbuf_ptr, uint64_t flags,
                               uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  struct kstat *ks = (struct kstat *)statbuf_ptr;

  if (!ks)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  // AT_EMPTY_PATH with empty string: behave like fstat(dirfd)
  if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0') {
    if ((int)dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
      return (uint64_t)-9; // EBADF
    fill_kstat(ks, t->fds[dirfd]);
    return 0;
  }

  if (!path)
    return (uint64_t)-14; // EFAULT

  // Resolve the base directory
  vfs_node_t *base_dir = fs_root;
  if (path[0] != '/') {
    if ((int)dirfd == AT_FDCWD) {
      // Use thread CWD
      if (t->cwd_path[0]) {
        base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base_dir)
          base_dir = fs_root;
      }
    } else {
      if ((int)dirfd < 0 || (int)dirfd >= MAX_FDS || !t->fds[dirfd])
        return (uint64_t)-9; // EBADF
      base_dir = t->fds[dirfd];
      if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    }
  }

  vfs_node_t *node = NULL;

  // Check device registry for /dev/ paths
  if (strncmp(path, "/dev/", 5) == 0) {
    node = fb_lookup_device((char *)path + 5);
  }

  if (!node) {
    node = vfs_resolve_path_at(base_dir, path);
    if (!node) {
      return (uint64_t)-2; // ENOENT
    }
  }

  fill_kstat(ks, node);
  return 0;
}
static vfs_node_t *resolve_parent_and_name(const char *path, char *name_out,
                                           size_t name_size) {
  if (!path)
    return NULL;

  size_t len = strlen(path);
  if (len == 0 || len >= 256)
    return NULL;

  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  if (t && path[0] != '/' && t->cwd_path[0]) {
    base = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (!base)
      base = fs_root;
  }

  // Find last slash
  const char *last_slash = NULL;
  for (const char *p = path; *p; p++)
    if (*p == '/')
      last_slash = p;

  vfs_node_t *parent;
  const char *basename;

  if (last_slash) {
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len == 0) {
      parent = fs_root;
    } else {
      char parent_path[256];
      if (parent_len >= sizeof(parent_path))
        return NULL;
      memcpy(parent_path, path, parent_len);
      parent_path[parent_len] = '\0';
      parent = vfs_resolve_path_at(base, parent_path);
    }
    basename = last_slash + 1;
  } else {
    parent = base;
    basename = path;
  }

  if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return NULL;

  size_t blen = strlen(basename);
  if (blen == 0 || blen >= name_size)
    return NULL;

  strcpy(name_out, basename);
  return parent;
}

// ── sys_symlink: symlink(target, linkpath) — syscall 88 ──────────────────────
static uint64_t sys_symlink(uint64_t target_ptr, uint64_t linkpath_ptr,
                            uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *target = (const char *)target_ptr;
  const char *linkpath = (const char *)linkpath_ptr;

  if (!target || !linkpath)
    return (uint64_t)-14; // EFAULT

  char link_name[128];
  vfs_node_t *parent =
      resolve_parent_and_name(linkpath, link_name, sizeof(link_name));
  if (!parent)
    return (uint64_t)-2; // ENOENT

  // Check if it already exists
  if (vfs_finddir(parent, link_name)) {
    return (uint64_t)-17; // EEXIST
  }

  // A local buffer for target string
  char target_buf[256];
  size_t t_len = strlen(target);
  if (t_len == 0 || t_len >= sizeof(target_buf))
    return (uint64_t)-14;
  strcpy(target_buf, target);

  int ret = vfs_symlink(parent, link_name, target_buf);
  if (ret != 0)
    return (uint64_t)-1;
  return 0;
}

// ── sys_unlink: unlink(pathname) — syscall 87 ────────────────────────────────
static uint64_t sys_unlink(uint64_t pathname_ptr, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  if (!path)
    return (uint64_t)-14; // EFAULT

  char name[128];
  vfs_node_t *parent = resolve_parent_and_name(path, name, sizeof(name));
  if (!parent)
    return (uint64_t)-2; // ENOENT

  // Check that the target exists and is not a directory
  vfs_node_t *target = vfs_finddir(parent, name);
  if (!target)
    return (uint64_t)-2; // ENOENT
  if ((target->flags & FS_TYPE_MASK) == FS_DIRECTORY)
    return (uint64_t)-21; // EISDIR

  // Invalidate any open fds pointing to this node
  struct thread *t = sched_get_current();
  if (t) {
    for (int i = 0; i < MAX_FDS; i++) {
      if (t->fds[i] == target) {
        t->fds[i] = NULL;
        t->fd_offsets[i] = 0;
      }
    }
  }

  // Unbind socket from internal list before filesystem unlink
  unix_unbind_by_path(path);

  int ret = vfs_unlink(parent, name);
  if (ret != 0)
    return (uint64_t)-1; // Generic error

  return 0;
}

// ── sys_rmdir: rmdir(pathname) — syscall 84 ──────────────────────────────────
static uint64_t sys_rmdir(uint64_t pathname_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  if (!path)
    return (uint64_t)-14; // EFAULT

  char name[128];
  vfs_node_t *parent = resolve_parent_and_name(path, name, sizeof(name));
  if (!parent)
    return (uint64_t)-2; // ENOENT

  vfs_node_t *target = vfs_finddir(parent, name);
  if (!target)
    return (uint64_t)-2; // ENOENT
  if ((target->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  int ret = vfs_rmdir(parent, name);
  if (ret != 0)
    return (uint64_t)-1;

  return 0;
}

// ── sys_chmod: chmod(pathname, mode) — syscall 90 ────────────────────────────
static uint64_t sys_chmod(uint64_t pathname_ptr, uint64_t mode, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  if (!path)
    return (uint64_t)-14;

  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  if (t && path[0] != '/' && t->cwd_path[0]) {
    base = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (!base)
      base = fs_root;
  }

  vfs_node_t *node = vfs_resolve_path_at(base, path);
  if (!node)
    return (uint64_t)-2;

  int ret = vfs_chmod(node, (uint16_t)mode);
  if (ret != 0)
    return (uint64_t)-1;
  return 0;
}

// ── sys_chown: chown(pathname, owner, group) — syscall 92 ────────────────────
static uint64_t sys_chown(uint64_t pathname_ptr, uint64_t owner, uint64_t group,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  if (!path)
    return (uint64_t)-14;

  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  if (t && path[0] != '/' && t->cwd_path[0]) {
    base = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (!base)
      base = fs_root;
  }

  vfs_node_t *node = vfs_resolve_path_at(base, path);
  if (!node)
    return (uint64_t)-2;

  uint32_t uid = (owner == (uint64_t)-1) ? node->uid : (uint32_t)owner;
  uint32_t gid = (group == (uint64_t)-1) ? node->gid : (uint32_t)group;

  int ret = vfs_chown(node, uid, gid);
  if (ret != 0)
    return (uint64_t)-1;
  return 0;
}

// ── sys_rename: rename(oldpath, newpath) — syscall 82 ────────────────────────
static uint64_t sys_rename(uint64_t oldpath_ptr, uint64_t newpath_ptr,
                           uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *oldpath = (const char *)oldpath_ptr;
  const char *newpath = (const char *)newpath_ptr;
  if (!oldpath || !newpath)
    return (uint64_t)-14; // EFAULT

  char old_name[128], new_name[128];
  vfs_node_t *old_parent =
      resolve_parent_and_name(oldpath, old_name, sizeof(old_name));
  vfs_node_t *new_parent =
      resolve_parent_and_name(newpath, new_name, sizeof(new_name));

  if (!old_parent)
    return (uint64_t)-2; // ENOENT (source path invalid)
  if (!new_parent)
    return (uint64_t)-2; // ENOENT (dest path invalid)

  // Check source exists
  vfs_node_t *source = vfs_finddir(old_parent, old_name);
  if (!source)
    return (uint64_t)-2; // ENOENT

  // Same directory rename (most common case, and what TCC uses)
  // Compare by inode, not pointer — ext2 finddir allocates new nodes per lookup
  if (old_parent == new_parent || old_parent->inode == new_parent->inode) {
    int ret = vfs_rename(old_parent, old_name, new_name);
    if (ret != 0)
      return (uint64_t)-1;
    return 0;
  }

  // Cross-directory rename is not supported in this simple VFS
  return (uint64_t)-18; // EXDEV
}

// ── sys_readlink: readlink(pathname, buf, bufsiz) — syscall 89 ───────────────
static uint64_t sys_readlink(uint64_t pathname_ptr, uint64_t buf_ptr,
                             uint64_t bufsiz, uint64_t a3, uint64_t a4,
                             uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  char *buf = (char *)buf_ptr;
  if (!path || !buf)
    return (uint64_t)-14; // EFAULT
  if (bufsiz == 0)
    return (uint64_t)-22; // EINVAL

  // Special case: /proc/self/exe — TCC and other tools read this
  if (strcmp(path, "/proc/self/exe") == 0) {
    // We don't track per-thread executable paths yet, so return a generic path
    const char *exe_path = "/init";
    size_t len = strlen(exe_path);
    if (len > bufsiz)
      len = bufsiz;
    memcpy(buf, exe_path, len);
    return len; // readlink returns bytes written, NOT null-terminated
  }

  // Resolve the symlink node
  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  if (t && path[0] != '/' && t->cwd_path[0]) {
    base = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (!base)
      base = fs_root;
  }

  vfs_node_t *node = vfs_resolve_path_at(base, path);
  if (!node)
    return (uint64_t)-2; // ENOENT

  // Must be a symlink
  if ((node->flags & FS_TYPE_MASK) != FS_SYMLINK)
    return (uint64_t)-22; // EINVAL — not a symlink

  int ret = vfs_readlink(node, buf, (uint32_t)bufsiz);
  if (ret < 0)
    return (uint64_t)-22; // EINVAL

  return (uint64_t)ret;
}

// ── sys_dup: dup(oldfd) — syscall 32
// ──────────────────────────────────────────
static uint64_t sys_dup(uint64_t oldfd, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || oldfd >= MAX_FDS || !t->fds[oldfd])
    return (uint64_t)-9; // EBADF

  int newfd = alloc_fd(t);
  if (newfd < 0)
    return (uint64_t)-24; // EMFILE

  t->fds[newfd] = t->fds[oldfd];
  t->fd_offsets[newfd] = t->fd_offsets[oldfd];
  vfs_open(t->fds[newfd]);

  return newfd;
}

static uint64_t sys_utimensat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                              uint64_t flags, uint64_t a4, uint64_t a5) {
  (void)dirfd;
  (void)pathname;
  (void)times;
  (void)flags;
  (void)a4;
  (void)a5;
  return 0; // successfully mocked
}

static uint64_t sys_futimesat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)dirfd;
  (void)pathname;
  (void)times;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0; // successfully mocked
}

static uint64_t sys_utimes(uint64_t pathname, uint64_t times, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)pathname;
  (void)times;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0; // successfully mocked
}

// ── sys_fchmod: fchmod(fd, mode) — syscall 91 ────────────────────────────────
static uint64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  node->mask = (uint32_t)(mode & 0777);
  return 0;
}

static uint64_t sys_link(uint64_t oldpath_ptr, uint64_t newpath_ptr,
                         uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *oldpath = (const char *)oldpath_ptr;
  const char *newpath = (const char *)newpath_ptr;
  if (!oldpath || !newpath)
    return (uint64_t)-14;
  vfs_node_t *src = vfs_resolve_path(oldpath);
  if (!src)
    return (uint64_t)-2;
  if ((src->flags & FS_TYPE_MASK) != FS_FILE)
    return (uint64_t)-1;

  char parent_path[128], file_name[128];
  size_t len = strlen(newpath);
  if (len == 0 || len >= sizeof(file_name))
    return (uint64_t)-36;

  const char *slash = 0;
  for (const char *p = newpath; *p; p++)
    if (*p == '/')
      slash = p;

  vfs_node_t *parent = fs_root;
  if (slash) {
    size_t parent_len = (size_t)(slash - newpath);
    if (parent_len > 0) {
      if (parent_len >= sizeof(parent_path))
        return (uint64_t)-36;
      memcpy(parent_path, newpath, parent_len);
      parent_path[parent_len] = '\0';
      parent = vfs_resolve_path(parent_path);
    }
    strcpy(file_name, slash + 1);
  } else {
    strcpy(file_name, newpath);
  }

  if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return (uint64_t)-20;
  if (vfs_finddir(parent, file_name))
    return (uint64_t)-17;
  if (vfs_create(parent, file_name, src->mask & 0777) != 0)
    return (uint64_t)-1;

  vfs_node_t *dst = vfs_finddir(parent, file_name);
  if (dst && src->length > 0) {
    uint8_t buf[512];
    uint32_t offset = 0;
    while (offset < src->length) {
      uint32_t chunk = src->length - offset;
      if (chunk > sizeof(buf))
        chunk = sizeof(buf);
      uint32_t rd = vfs_read(src, offset, chunk, buf);
      if (rd == 0)
        break;
      vfs_write(dst, offset, rd, buf);
      offset += rd;
    }
  }
  return 0;
}

struct pollfd {
  int fd;
  short events;
  short revents;
};

static uint64_t do_poll(struct pollfd *fds, uint64_t nfds,
                        uint64_t timeout_ms) {
  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  int ready = 0;
  for (uint64_t i = 0; i < nfds; i++) {
    int fd = fds[i].fd;
    if (fd < 0) {
      fds[i].revents = 0;
      continue;
    }
    if (fd >= MAX_FDS || !t->fds[fd]) {
      fds[i].revents = POLLNVAL;
      ready++;
      continue;
    }
    int ret = vfs_poll(t->fds[fd], fds[i].events);
    if (ret < 0) {
      fds[i].revents = POLLNVAL;
      ready++;
    } else if (ret > 0) {
      fds[i].revents = (short)ret;
      ready++;
    } else {
      fds[i].revents = 0;
    }
  }

  if (ready == 0 && timeout_ms != 0) {
    // Create wait queue entries for each fd we're polling
    // Each fd needs its own entry to avoid list corruption
    wait_queue_entry_t entries[128]; // Max 128 fds per poll
    int entry_fds[128];              // Track which fd each entry belongs to
    int entry_count = 0;

    // Add to all fd wait queues so we get woken when any has events
    for (uint64_t i = 0; i < nfds && entry_count < 128; i++) {
      int fd = fds[i].fd;
      if (fd >= 0 && fd < MAX_FDS && t->fds[fd] && t->fds[fd]->wait_queue) {
        entries[entry_count].thread = t;
        entries[entry_count].next = NULL;
        entry_fds[entry_count] = fd;
        wait_queue_add((wait_queue_t *)t->fds[fd]->wait_queue,
                       &entries[entry_count]);
        entry_count++;
      }
    }

    // Set up timeout if specified
    if (timeout_ms != (uint64_t)-1) {
      t->wakeup_ticks = lapic_timer_get_ticks() + timeout_ms;
    } else {
      t->wakeup_ticks = 0; // No timeout
    }

    // Set state to BLOCKED BEFORE the final check to avoid lost wakeups
    t->state = THREAD_BLOCKED;

    // Final check for ready fds after setting state
    for (uint64_t i = 0; i < nfds; i++) {
      int fd = fds[i].fd;
      if (fd >= 0 && fd < MAX_FDS && t->fds[fd]) {
        if (vfs_poll(t->fds[fd], fds[i].events) > 0) {
          t->state = THREAD_READY;
          ready = 1; // Mark as ready so we don't block
          break;
        }
      }
    }

    // Only yield if we're still blocked
    if (t->state == THREAD_BLOCKED) {
      sched_yield();
    }

    // Remove from all wait queues
    for (int i = 0; i < entry_count; i++) {
      int fd = entry_fds[i];
      if (fd >= 0 && fd < MAX_FDS && t->fds[fd] && t->fds[fd]->wait_queue) {
        wait_queue_remove((wait_queue_t *)t->fds[fd]->wait_queue, &entries[i]);
      }
    }

    // Re-poll to get actual events
    for (uint64_t i = 0; i < nfds; i++) {
      int fd = fds[i].fd;
      if (fd < 0 || fd >= MAX_FDS || !t->fds[fd])
        continue;
      int ret = vfs_poll(t->fds[fd], fds[i].events);
      if (ret > 0) {
        fds[i].revents = (short)ret;
        ready++;
      }
    }
  }
  return (uint64_t)ready;
}

static uint64_t sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ms,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  if (nfds == 0) {
    if (timeout_ms > 0 && timeout_ms != (uint64_t)-1) {
      t->state = THREAD_SLEEPING;
      uint64_t wait = (timeout_ms == (uint64_t)-1) ? 10 : timeout_ms;
      if (wait == 0)
        wait = 1;
      t->wakeup_ticks = lapic_timer_get_ticks() + wait;
      sched_yield();
    }
    return 0;
  }
  if (!is_user_ptr(fds_ptr) ||
      !vmm_is_user_addr_range_valid(fds_ptr, nfds * sizeof(struct pollfd)))
    return (uint64_t)-14;

  return do_poll((struct pollfd *)fds_ptr, nfds, timeout_ms);
}

static uint64_t sys_ppoll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ptr,
                          uint64_t sigmask, uint64_t sigsetsize, uint64_t a5) {
  (void)sigmask;
  (void)sigsetsize;
  (void)a5;

  uint64_t timeout_ms = (uint64_t)-1;
  if (timeout_ptr && is_user_ptr(timeout_ptr)) {
    struct {
      int64_t tv_sec;
      int64_t tv_nsec;
    } *ts = (void *)timeout_ptr;
    if (!vmm_is_user_addr_range_valid(timeout_ptr, 16))
      return (uint64_t)-14;
    timeout_ms = (uint64_t)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
  }

  return sys_poll(fds_ptr, nfds, timeout_ms, 0, 0, 0);
}

static uint64_t do_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                            uint64_t exceptfds, uint64_t timeout_ms) {
  size_t set_size = (nfds + 7) / 8;
  struct pollfd pfds[128];
  uint64_t p_count = 0;
  for (int fd = 0; fd < (int)nfds && p_count < 128; fd++) {
    short events = 0;
    if (readfds && (((uint64_t *)readfds)[fd / 64] & (1ULL << (fd % 64))))
      events |= 0x0001;
    if (writefds && (((uint64_t *)writefds)[fd / 64] & (1ULL << (fd % 64))))
      events |= 0x0004;
    if (events) {
      pfds[p_count].fd = fd;
      pfds[p_count].events = events;
      pfds[p_count].revents = 0;
      p_count++;
    }
  }

  if (p_count == 0) {
    if (timeout_ms != (uint64_t)-1 && timeout_ms > 0) {
      struct thread *t = sched_get_current();
      t->state = THREAD_SLEEPING;
      uint64_t wait = (timeout_ms == (uint64_t)-1) ? 10 : timeout_ms;
      if (wait == 0)
        wait = 1;
      t->wakeup_ticks = lapic_timer_get_ticks() + wait;
      sched_yield();
    }
    if (readfds)
      memset((void *)readfds, 0, set_size);
    if (writefds)
      memset((void *)writefds, 0, set_size);
    if (exceptfds)
      memset((void *)exceptfds, 0, set_size);
    return 0;
  }

  uint64_t ready = do_poll(pfds, p_count, timeout_ms);
  if ((int64_t)ready < 0)
    return ready;

  if (readfds)
    memset((void *)readfds, 0, set_size);
  if (writefds)
    memset((void *)writefds, 0, set_size);
  if (exceptfds)
    memset((void *)exceptfds, 0, set_size);

  uint64_t res_count = 0;
  for (uint64_t i = 0; i < p_count; i++) {
    if (pfds[i].revents == 0)
      continue;
    int fd = pfds[i].fd;
    if (pfds[i].revents & (0x0001 | 0x0010 | 0x0008)) {
      if (readfds)
        ((uint64_t *)readfds)[fd / 64] |= (1ULL << (fd % 64));
      res_count++;
    }
    if (pfds[i].revents & 0x0004) {
      if (writefds)
        ((uint64_t *)writefds)[fd / 64] |= (1ULL << (fd % 64));
      res_count++;
    }
  }
  return res_count;
}

static uint64_t sys_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                             uint64_t exceptfds, uint64_t timeout,
                             uint64_t sigmask) {
  (void)sigmask;
  if (nfds > 1024)
    return (uint64_t)-22;
  size_t set_size = (nfds + 7) / 8;
  if (readfds && (!is_user_ptr(readfds) ||
                  !vmm_is_user_addr_range_valid(readfds, set_size)))
    return (uint64_t)-14;
  if (writefds && (!is_user_ptr(writefds) ||
                   !vmm_is_user_addr_range_valid(writefds, set_size)))
    return (uint64_t)-14;
  if (exceptfds && (!is_user_ptr(exceptfds) ||
                    !vmm_is_user_addr_range_valid(exceptfds, set_size)))
    return (uint64_t)-14;

  uint64_t timeout_ms = (uint64_t)-1;
  if (timeout && is_user_ptr(timeout)) {
    if (!vmm_is_user_addr_range_valid(timeout, 16))
      return (uint64_t)-14;
    struct {
      int64_t tv_sec;
      int64_t tv_nsec;
    } *ts = (void *)timeout;
    timeout_ms = (uint64_t)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
  }

  return do_pselect6(nfds, readfds, writefds, exceptfds, timeout_ms);
}

static uint64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                           uint64_t exceptfds, uint64_t timeout, uint64_t a5) {
  (void)a5;
  uint64_t timeout_ms = (uint64_t)-1;

  if (timeout && is_user_ptr(timeout)) {
    struct {
      int64_t tv_sec;
      int64_t tv_usec;
    } *tv = (void *)timeout;
    if (!vmm_is_user_addr_range_valid(timeout, 16))
      return (uint64_t)-14;
    timeout_ms = (uint64_t)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
  }

  return do_pselect6(nfds, readfds, writefds, exceptfds, timeout_ms);
}

// fchdir(2) - syscall 81
// Change current directory using file descriptor
static uint64_t sys_fchdir(uint64_t fd, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1; // EACCES

  // Validate fd exists
  if (fd >= MAX_FDS)
    return (uint64_t)-9; // EBADF

  if (!t->fds[fd])
    return (uint64_t)-9; // EBADF

  // Check if it's a directory
  if ((t->fds[fd]->flags & FS_TYPE_MASK) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  // Change to the directory stored in fd_paths[fd]
  if (t->fd_paths[fd][0]) {
    strncpy(t->cwd_path, t->fd_paths[fd], sizeof(t->cwd_path) - 1);
    t->cwd_path[sizeof(t->cwd_path) - 1] = '\0';
    
    klog_puts("[FCHDIR] Changed cwd to: ");
    klog_puts(t->cwd_path);
    klog_puts(" via fd=");
    klog_uint64(fd);
    klog_puts("\n");
    
    return 0; // Success
  }

  return (uint64_t)-9; // EBADF - no path associated with fd
}

// statfs(2) - syscall 137
// Returns filesystem statistics for a given path
static uint64_t sys_statfs(uint64_t path_ptr, uint64_t buf_ptr, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;

  if (!path_ptr || !buf_ptr)
    return (uint64_t)-14; // EFAULT
  if (!is_user_ptr(path_ptr) || !is_user_ptr(buf_ptr))
    return (uint64_t)-14; // EFAULT

  // statfs structure (Linux x86_64)
  struct statfs_buf {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint64_t f_fsid[2];
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
  } *buf = (struct statfs_buf *)buf_ptr;

  // Basic stub implementation - return dummy values
  buf->f_type = 0x61657673;    // "aev" in hex - custom filesystem type
  buf->f_bsize = 4096;         // 4K block size
  buf->f_blocks = 1024 * 256;  // ~1GB total
  buf->f_bfree = 1024 * 128;   // ~512MB free
  buf->f_bavail = 1024 * 128;  // ~512MB available to user
  buf->f_files = 10000;        // max inodes
  buf->f_ffree = 5000;         // free inodes
  buf->f_fsid[0] = 1;
  buf->f_fsid[1] = 0;
  buf->f_namelen = 255;
  buf->f_frsize = 4096;
  buf->f_flags = 0;

  return 0; // Success
}

// inotify_init(2) - syscall 253 (x86_64 AscentOS)
// Initialize an inotify instance
static uint64_t sys_inotify_init(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  // Find an available inotify instance slot
  inotify_instance_t *instance = NULL;
  for (int i = 0; i < MAX_INOTIFY_INSTANCES; i++) {
    if (inotify_instances[i].instance_id == 0) {
      instance = &inotify_instances[i];
      break;
    }
  }

  if (!instance)
    return (uint64_t)-23; // ENFILE - too many open files

  // Initialize the instance
  instance->instance_id = next_instance_id++;
  instance->num_watches = 0;
  instance->queue_head = 0;
  instance->queue_tail = 0;

  // Allocate a file descriptor for this inotify instance
  int fd = alloc_fd(t);
  if (fd < 0)
    return (uint64_t)-24; // EMFILE

  // Create a special vfs_node to represent this inotify instance
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return (uint64_t)-12; // ENOMEM

  vfs_node_init(node);
  node->flags = FS_CHARDEV; // Treat as character device
  node->mask = 0600;
  node->impl = instance->instance_id; // Store instance ID in impl field
  node->name[0] = '\0';
  strcat(node->name, "inotify");

  vfs_open(node);
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;
  
  // Build fd_path string: "inotify" (instance ID is stored in node->impl)
  strcpy(t->fd_paths[fd], "inotify");

  klog_puts("[INOTIFY_INIT] Created instance ");
  klog_uint64(instance->instance_id);
  klog_puts(" with fd=");
  klog_uint64(fd);
  klog_puts("\n");

  return fd;
}

// inotify_init1(2) - syscall 294 (x86_64)
// Initialize an inotify instance with flags
static uint64_t sys_inotify_init1(uint64_t flags, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  (void)a6;
  // Flags: IN_NONBLOCK (0x800), IN_CLOEXEC (0x80000)
  (void)flags;

  // For now, same implementation as inotify_init (ignoring flags)
  return sys_inotify_init(0, 0, 0, 0, 0, 0);
}

// inotify_add_watch(2) - syscall 254 (x86_64)
// Add a watch to an inotify instance
static uint64_t sys_inotify_add_watch(uint64_t fd, uint64_t pathname,
                                      uint64_t mask, uint64_t a4, uint64_t a5,
                                      uint64_t a6) {
  (void)a4;
  (void)a5;
  (void)a6;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (!pathname)
    return (uint64_t)-14; // EFAULT
  if (!is_user_ptr(pathname))
    return (uint64_t)-14; // EFAULT

  // Validate inotify fd
  if (fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if (strcmp(node->name, "inotify") != 0)
    return (uint64_t)-22; // EINVAL - not an inotify fd

  // Get the inotify instance
  uint32_t instance_id = node->impl;
  inotify_instance_t *instance = NULL;
  for (int i = 0; i < MAX_INOTIFY_INSTANCES; i++) {
    if (inotify_instances[i].instance_id == instance_id) {
      instance = &inotify_instances[i];
      break;
    }
  }

  if (!instance)
    return (uint64_t)-22; // EINVAL

  // Check if we have space for another watch
  if (instance->num_watches >= MAX_INOTIFY_WATCHES)
    return (uint64_t)-23; // ENFILE

  const char *path = (const char *)pathname;

  // Check if this path is already being watched
  for (uint32_t i = 0; i < instance->num_watches; i++) {
    if (strcmp(instance->watches[i].path, path) == 0) {
      // Update the mask for existing watch
      instance->watches[i].mask = (uint32_t)mask;
      klog_puts("[INOTIFY_ADD_WATCH] Updated watch for path: ");
      klog_puts(path);
      klog_puts("\n");
      return instance->watches[i].wd;
    }
  }

  // Add new watch
  inotify_watch_t *watch = &instance->watches[instance->num_watches];
  watch->wd = next_watch_id++;
  watch->mask = (uint32_t)mask;
  watch->event_mask = 0;
  strncpy(watch->path, path, sizeof(watch->path) - 1);
  watch->path[sizeof(watch->path) - 1] = '\0';

  instance->num_watches++;

  klog_puts("[INOTIFY_ADD_WATCH] Added watch wd=");
  klog_uint64(watch->wd);
  klog_puts(" for path: ");
  klog_puts(path);
  klog_puts(" in instance ");
  klog_uint64(instance_id);
  klog_puts("\n");

  return watch->wd;
}

void syscall_register_io(void) {
  syscall_register(SYS_READ, sys_read);
  syscall_register(SYS_WRITE, sys_write);
  syscall_register(SYS_READV, sys_readv);
  syscall_register(SYS_WRITEV, sys_writev);
  syscall_register(SYS_IOCTL, sys_ioctl);
  syscall_register(SYS_OPEN, sys_open);
  syscall_register(SYS_CLOSE, sys_close);
  syscall_register(SYS_POLL, sys_poll);
  syscall_register(SYS_PPOLL, sys_ppoll);
  syscall_register(SYS_LSEEK, sys_lseek);
  syscall_register(SYS_MKDIR, sys_mkdir);
  syscall_register(SYS_MKDIRAT, sys_mkdirat);
  syscall_register(SYS_UNLINKAT, sys_unlinkat);
  syscall_register(SYS_FTRUNCATE, sys_ftruncate);
  syscall_register(SYS_FCNTL, sys_fcntl);
  syscall_register(SYS_STAT, sys_stat);
  syscall_register(SYS_FSTAT, sys_fstat);
  syscall_register(SYS_LSTAT, sys_lstat);
  syscall_register(SYS_GETDENTS64, sys_getdents64);
  syscall_register(SYS_GETDENTS, sys_getdents);
  syscall_register(SYS_PSELECT6, sys_pselect6);
  syscall_register(SYS_SELECT, sys_select);
  syscall_register(SYS_PIPE, sys_pipe);
  syscall_register(SYS_PIPE2, sys_pipe2);
  syscall_register(SYS_EVENTFD2, sys_eventfd2);
  syscall_register(SYS_GETRANDOM, sys_getrandom);
  syscall_register(SYS_ACCESS, sys_access);
  syscall_register(SYS_FACCESSAT2, sys_faccessat2);
  syscall_register(SYS_OPENAT, sys_openat);
  syscall_register(SYS_FCHMODAT, sys_fchmodat);
  syscall_register(SYS_FCHOWNAT, sys_fchownat);
  syscall_register(SYS_FCHDIR, sys_fchdir);
  syscall_register(SYS_NEWFSTATAT, sys_newfstatat);
  syscall_register(SYS_UNLINK, sys_unlink);
  syscall_register(SYS_RENAME, sys_rename);
  syscall_register(SYS_SYMLINK, sys_symlink);
  syscall_register(SYS_READLINK, sys_readlink);
  syscall_register(SYS_DUP, sys_dup);
  syscall_register(SYS_DUP2, sys_dup2);
  syscall_register(SYS_RMDIR, sys_rmdir);
  syscall_register(SYS_CHMOD, sys_chmod);
  syscall_register(SYS_CHOWN, sys_chown);
  syscall_register(SYS_STATX, sys_statx);
  syscall_register(SYS_UTIMENSAT, sys_utimensat);
  syscall_register(SYS_FUTIMESAT, sys_futimesat);
  syscall_register(SYS_UTIMES, sys_utimes);
  syscall_register(SYS_FCHMOD, sys_fchmod);
  syscall_register(SYS_LINK, sys_link);
  syscall_register(SYS_FADVISE64, sys_fadvise64);
  syscall_register(SYS_STATFS, sys_statfs);
  syscall_register(SYS_INOTIFY_INIT, sys_inotify_init);
  syscall_register(SYS_INOTIFY_INIT1, sys_inotify_init1);
  syscall_register(SYS_INOTIFY_ADD_WATCH, sys_inotify_add_watch);

  console_termios.c_lflag = 0x0000000b;
  console_termios.c_iflag = 0x00000100;
  console_termios.c_oflag = 0x00000005;
}
