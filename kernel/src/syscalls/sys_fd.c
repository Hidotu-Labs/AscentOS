// sys_fd.c — Core file-descriptor syscalls:
//   open, openat, close, dup, dup2, read, write, readv, writev,
//   sendfile, lseek, fcntl, flock, ftruncate, fallocate, fsync, fadvise64
#include "../apic/lapic_timer.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../fb/framebuffer.h"
#include "../drivers/gpu/drm/drm.h"
#include "../font/font.h"
#include "../fs/procfs.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../socket/socket.h"
#include "sys_io_shared.h"
#include "syscall.h"
#include "../fs/ext2.h"
#include "../fs/ext4.h"
#include "../drivers/storage/block.h"
#include "arch/uaccess.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// FD allocation helpers
// ---------------------------------------------------------------------------

#define FD_RESERVED ((vfs_node_t *)-1)

int alloc_fd(struct thread *t) {
  if (!t || !t->files)
    return -1;
  spinlock_acquire(&t->files->lock);
  for (int i = (int)t->files->next_fd; i < MAX_FDS; i++) {
    if (t->fds[i] == NULL) {
      t->fds[i] = FD_RESERVED;
      t->files->next_fd = (uint32_t)i + 1;
      spinlock_release(&t->files->lock);
      return i;
    }
  }
  spinlock_release(&t->files->lock);
  return -1;
}

int alloc_fd_from(struct thread *t, int from) {
  if (!t || !t->files || from < 0 || from >= MAX_FDS)
    return -1;
  spinlock_acquire(&t->files->lock);
  int start = from;
  if ((uint32_t)start < t->files->next_fd)
    start = (int)t->files->next_fd;
  for (int i = start; i < MAX_FDS; i++) {
    if (t->fds[i] == NULL) {
      t->fds[i] = FD_RESERVED;
      t->files->next_fd = (uint32_t)i + 1;
      spinlock_release(&t->files->lock);
      return i;
    }
  }
  spinlock_release(&t->files->lock);
  return -1;
}

// ---------------------------------------------------------------------------
// open / openat
// ---------------------------------------------------------------------------

uint64_t sys_open_path(int dirfd, const char *path, uint64_t flags,
                            uint64_t mode) {
  (void)dirfd;
  if (!path)
    return (uint64_t)-14; // EFAULT

  (void)flags;
  (void)mode;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  int fd;

  vfs_node_t *base_dir = fs_root;
  if (path[0] != '/') {
    if (dirfd == AT_FDCWD) {
      base_dir = t->cwd_node ? t->cwd_node : fs_root;
    } else {
      if (dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
        return (uint64_t)-9; // EBADF
      base_dir = t->fds[dirfd];
      if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    }
  }

  vfs_node_t *node = NULL;
  const char *dev_path = NULL;
  if (strncmp(path, "/dev/", 5) == 0)
    dev_path = path + 5;
  else if (strncmp(path, "dev/", 4) == 0)
    dev_path = path + 4;

  if (dev_path) {
    if (strcmp(dev_path, "dri/card0") == 0) {
      node = drm_create_client_node();
      if (!node)
        return (uint64_t)-12;
    }

    if (strcmp(dev_path, "ptmx") == 0) {
      int pty_index = pty_alloc_pair();
      if (pty_index < 0)
        return (uint64_t)-16; // EBUSY

      pty_pair_t *pty = pty_get_pair(pty_index);
      if (!pty)
        return (uint64_t)-16;

      node = kmalloc(sizeof(vfs_node_t));
      if (!node)
        return (uint64_t)-12; // ENOMEM
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
      pty->master_node = node;
      klog_puts("[PTYMASTER] pair=");
      klog_uint64((uint64_t)pty->index);
      // The descriptor installed below owns the initial reference.
      node->refcount = 0;
      goto open_done;
    }

    if (strncmp(dev_path, "pts/", 4) == 0) {
      const char *num_str = dev_path + 4;
      int pty_index = 0;
      while (*num_str >= '0' && *num_str <= '9') {
        pty_index = pty_index * 10 + (*num_str - '0');
        num_str++;
      }
      if (*num_str != '\0')
        return (uint64_t)-2;

      pty_pair_t *pty = pty_get_pair(pty_index);
      if (!pty || pty->locked)
        return (uint64_t)-2;

      node = kmalloc(sizeof(vfs_node_t));
      if (!node)
        return (uint64_t)-12;
      vfs_node_init(node);
      strcpy(node->name, dev_path);
      node->flags = FS_CHARDEV;
      node->mask = 0620;
      node->uid = t->euid;
      node->gid = t->egid;
      node->read = pty_slave_read;
      node->write = pty_slave_write;
      node->ioctl = pty_slave_ioctl;
      node->poll = pty_slave_poll;
      node->open = pty_slave_open;
      node->close = pty_slave_close;
      node->mmap = pty_slave_mmap;
      node->device = pty;
      node->wait_queue = pty->slave_waitq;
      // The descriptor installed below owns the initial reference.
      node->refcount = 0;
      goto open_done;
    }

    if (strcmp(dev_path, "tty") == 0) {
      struct thread *ct = sched_get_current();
      if (ct && ct->ctty) {
        node = ct->ctty;
        goto open_done;
      }
    }

    if (!node)
      node = fb_lookup_device((char *)dev_path);
  }

  if (!node)
    node = vfs_resolve_path_at(base_dir, path);

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
        return (uint64_t)-20;
      if (!vfs_access(parent, 3)) return (uint64_t)-13;

      mode &= ~t->umask;
      if (vfs_create(parent, file_name, (uint16_t)mode) != 0)
        return (uint64_t)-17;

      node = vfs_finddir(parent, file_name);
      if (!node) return (uint64_t)-2;
      uint32_t new_gid = (parent->mask & 02000) ? parent->gid : t->fsgid;
      vfs_chown(node, t->fsuid, new_gid);
    } else {
      return (uint64_t)-2; // ENOENT
    }
  }

  uint32_t requested = 0;
  if ((flags & O_ACCMODE) == O_RDONLY) requested = 4;
  else if ((flags & O_ACCMODE) == O_WRONLY) requested = 2;
  else if ((flags & O_ACCMODE) == O_RDWR) requested = 6;
  if (requested && !vfs_access(node, requested)) return (uint64_t)-13;

  if ((flags & O_TRUNC) && (node->flags & FS_TYPE_MASK) == FS_FILE) {
    vfs_truncate(node, 0);
    node->length = 0;
  }

  /* Persistent metadata may provide a fresh per-open object.  DRM was the
   * first user of this pattern; driver capability handles use it as well. */
  if (node->open_instance) {
    node = node->open_instance(node);
    if (!node)
      return (uint64_t)-12;
  } else if (drm_is_card_node(node)) {
    node = drm_create_client_node();
    if (!node)
      return (uint64_t)-12;
  }

open_done:
  fd = alloc_fd(t);
  if (fd < 0)
    return (uint64_t)-24; // EMFILE

  vfs_open(node);
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;
  t->fd_flags[fd] = flags & ~(uint64_t)O_CLOEXEC;
  if (flags & O_CLOEXEC)
    t->fd_flags[fd] |= FD_FLAGS_CLOEXEC_BIT;
  if (strcmp(node->name, "ptmx") == 0) {
    klog_puts(" fd=");
    klog_uint64((uint64_t)fd);
    klog_puts(" tid=");
    klog_uint64(t->tid);
    klog_puts("\n");
  }

  char full_path[256];
  if (path[0] == '/') {
    strncpy(full_path, path, sizeof(full_path) - 1);
    full_path[sizeof(full_path) - 1] = '\0';
  } else {
    full_path[0] = '\0';
    if (t->cwd_path[0] && strcmp(t->cwd_path, "/") != 0) {
      strncpy(full_path, t->cwd_path, sizeof(full_path) - 1);
      full_path[sizeof(full_path) - 1] = '\0';
      strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
      strncat(full_path, path, sizeof(full_path) - strlen(full_path) - 1);
    } else {
      strcpy(full_path, "/");
      strncat(full_path, path, sizeof(full_path) - 2);
    }
    full_path[sizeof(full_path) - 1] = '\0';
  }
  fd_path_set(t, fd, full_path);

  return fd;
}

static uint64_t sys_openat(uint64_t dirfd, uint64_t path_ptr, uint64_t flags,
                           uint64_t mode, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  return (uint64_t)(int)sys_open_path((int)dirfd, (const char *)path_ptr, flags,
                                    mode);
}

static uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return sys_openat(AT_FDCWD, path_ptr, flags, mode, 0, 0);
}

// ---------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------

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

  spinlock_acquire(&t->files->lock);
  vfs_node_t *node = t->fds[fd];
  t->fds[fd] = NULL;
  fd_path_clear(t, (int)fd);
  if (fd < t->files->next_fd)
    t->files->next_fd = (uint32_t)fd;
  spinlock_release(&t->files->lock);
  vfs_close(node);
  return 0;
}

/* Linux close_range(2).  VTE uses this while preparing its shell child. */
#define CLOSE_RANGE_UNSHARE (1U << 1)
#define CLOSE_RANGE_CLOEXEC (1U << 2)
static uint64_t sys_close_range(uint64_t first, uint64_t last, uint64_t flags,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (first > last || (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)))
    return (uint64_t)-22;

  struct thread *t = sched_get_current();
  if (!t || !t->files)
    return (uint64_t)-9;
  klog_puts("[CLOSE_RANGE] tid=");
  klog_uint64(t->tid);
  klog_puts(" first=");
  klog_uint64(first);
  klog_puts(" last=");
  klog_uint64(last);
  klog_puts(" flags=");
  klog_hex64(flags);
  klog_puts("\n");
  if (first >= MAX_FDS)
    return 0;
  if (last >= MAX_FDS)
    last = MAX_FDS - 1;

  /* File tables are already private for forked children.  CLONE_UNSHARE is
   * accepted here; callers using it only need the range operation itself. */
  if (flags & CLOSE_RANGE_CLOEXEC) {
    spinlock_acquire(&t->files->lock);
    for (uint64_t fd = first; fd <= last; fd++) {
      if (t->fds[fd] && t->fds[fd] != FD_RESERVED)
        t->fd_flags[fd] |= FD_FLAGS_CLOEXEC_BIT;
    }
    spinlock_release(&t->files->lock);
    return 0;
  }

  for (uint64_t fd = first; fd <= last; fd++) {
    spinlock_acquire(&t->files->lock);
    vfs_node_t *node = t->fds[fd];
    if (!node || node == FD_RESERVED) {
      spinlock_release(&t->files->lock);
      continue;
    }
    t->fds[fd] = NULL;
    t->fd_offsets[fd] = 0;
    t->fd_flags[fd] = 0;
    fd_path_clear(t, (int)fd);
    if (fd < t->files->next_fd)
      t->files->next_fd = (uint32_t)fd;
    spinlock_release(&t->files->lock);
    vfs_close(node);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// dup / dup2
// ---------------------------------------------------------------------------

static uint64_t sys_dup(uint64_t oldfd, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || oldfd >= MAX_FDS || !t->fds[oldfd])
    return (uint64_t)-9;

  int newfd = alloc_fd(t);
  if (newfd < 0)
    return (uint64_t)-24;

  t->fds[newfd] = t->fds[oldfd];
  t->fd_offsets[newfd] = t->fd_offsets[oldfd];
  t->fd_flags[newfd] = t->fd_flags[oldfd] & ~(uint64_t)FD_FLAGS_CLOEXEC_BIT;
  fd_path_dup(t, newfd, (int)oldfd);
  vfs_open(t->fds[newfd]);
  return newfd;
}

static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || oldfd >= MAX_FDS || newfd >= MAX_FDS || !t->fds[oldfd])
    return (uint64_t)-9;
  if (oldfd == newfd)
    return newfd;
  if (t->fds[newfd])
    vfs_close(t->fds[newfd]);

  t->fds[newfd] = t->fds[oldfd];
  t->fd_offsets[newfd] = t->fd_offsets[oldfd];
  t->fd_flags[newfd] = t->fd_flags[oldfd] & ~(uint64_t)FD_FLAGS_CLOEXEC_BIT;
  fd_path_dup(t, (int)newfd, (int)oldfd);
  vfs_open(t->fds[newfd]);
  return newfd;
}

// ---------------------------------------------------------------------------
// read / write helpers
// ---------------------------------------------------------------------------

static void trace_userspace_debug_write(struct thread *t, int fd,
                                        const void *buf, size_t count) {
  if (!t || fd < 0 || fd >= MAX_FDS || !t->fds[fd] || !buf || count == 0)
    return;

  const char *prefix = NULL;
  if (strcmp(t->fds[fd]->name, "weston-debug.log") == 0)
    prefix = "[WESTON-LOG] ";
  else if (strcmp(t->fds[fd]->name, "xfwm4.log") == 0)
    prefix = "[XFWM4-LOG] ";
  else
    return;

  klog_puts(prefix);
  const char *s = (const char *)buf;
  for (size_t i = 0; i < count; i++) {
    char c = s[i];
    if (c == '\0')
      break;
    klog_putchar(c);
  }
  if (((const char *)buf)[count - 1] != '\n')
    klog_putchar('\n');
}

static int64_t fd_write(int fd, const void *buf, size_t count) {
  struct thread *t = sched_get_current();
  if (!t || fd < 0 || fd >= MAX_FDS || !t->fds[fd])
    return -9;

  vfs_node_t *node = t->fds[fd];
  trace_userspace_debug_write(t, fd, buf, count);
  int32_t bytes_written =
      (int32_t)vfs_write(node, t->fd_offsets[fd], count, (uint8_t *)buf);
  if (bytes_written > 0)
    t->fd_offsets[fd] += (uint32_t)bytes_written;
  return (int64_t)bytes_written;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  int32_t bytes_read =
      (int32_t)vfs_read(node, t->fd_offsets[fd], count, (uint8_t *)buf);
  if (bytes_read > 0)
    t->fd_offsets[fd] += (uint32_t)bytes_read;

  return (uint64_t)(int64_t)bytes_read;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;
  return (uint64_t)fd_write((int)fd, (const void *)buf, (size_t)count);
}

static uint64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count,
                            uint64_t offset, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  int32_t bytes_read =
      (int32_t)vfs_read(node, (uint32_t)offset, count, (uint8_t *)buf);
  return (uint64_t)(int64_t)bytes_read;
}

static uint64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t count,
                             uint64_t offset, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!is_user_range((const void *)buf, count))
    return (uint64_t)-14;
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  int32_t bytes_written =
      (int32_t)vfs_write(node, (uint32_t)offset, count, (uint8_t *)buf);
  return (uint64_t)(int64_t)bytes_written;
}

static uint64_t sys_readv(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return (uint64_t)-22;

  struct user_iovec *iov = (struct user_iovec *)iov_u;
  if (!is_user_range((const void *)iov_u, iovcnt * sizeof(*iov)))
    return (uint64_t)-14;

  size_t total = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;
    if (!is_user_range((const void *)base, len))
      return (uint64_t)-14;

    struct thread *ct = sched_get_current();
    if (!ct || fd >= MAX_FDS || !ct->fds[fd])
      return (uint64_t)-9;

    vfs_node_t *node = ct->fds[fd];
    int32_t bytes_read =
        (int32_t)vfs_read(node, ct->fd_offsets[fd], len, (uint8_t *)base);
    if (bytes_read < 0) {
      if (total > 0)
        break;
      return (uint64_t)(int64_t)bytes_read;
    }
    if (bytes_read > 0)
      ct->fd_offsets[fd] += (uint32_t)bytes_read;
    total += (size_t)bytes_read;
    if ((uint32_t)bytes_read < len)
      break;
  }
  return total;
}

static uint64_t sys_writev(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return (uint64_t)-22;

  struct user_iovec *iov = (struct user_iovec *)iov_u;
  if (!is_user_range((const void *)iov_u, iovcnt * sizeof(*iov)))
    return (uint64_t)-14;

  for (uint64_t i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len > 0 &&
        !is_user_range((const void *)iov[i].iov_base, iov[i].iov_len))
      return (uint64_t)-14;
  }

  /* A socket writev is one send operation.  Splitting it into write calls can
   * interleave vector fragments with another writer and corrupt protocols. */
  socket_t *sock = socket_from_fd((int)fd);
  if (sock && socket_try_get(sock)) {
    if (sock->closing) {
      socket_put(sock);
      return (uint64_t)-32;
    }
    struct msghdr msg = {0};
    msg.msg_iov = (struct iovec *)iov;
    msg.msg_iovlen = (size_t)iovcnt;
    ssize_t ret = sock->ops && sock->ops->sendmsg
                      ? sock->ops->sendmsg(sock, &msg, 0)
                      : -95;
    socket_put(sock);
    return (uint64_t)ret;
  }

  size_t total = 0;
  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;

    int64_t w = fd_write((int)fd, (const void *)base, (size_t)len);
    if (w < 0)
      return total > 0 ? total : (uint64_t)w;
    if (w == 0 && len != 0) {
      /* A non-empty writev must not report a zero-byte success: callers such
       * as GNU ld retry it forever. Leave a focused diagnostic while finding
       * the backing filesystem path responsible for the short write. */
      struct thread *t = sched_get_current();
      klog_puts("[WRITEV] zero write fd=");
      klog_uint64(fd);
      klog_puts(" len=");
      klog_uint64(len);
      klog_puts(" node=");
      if (t && fd < MAX_FDS && t->fds[fd])
        klog_puts(t->fds[fd]->name);
      else
        klog_puts("(invalid)");
      klog_puts(" flags=");
      if (t && fd < MAX_FDS && t->fds[fd])
        klog_uint64(t->fds[fd]->flags);
      else
        klog_puts("0");
      klog_puts("\n");
      return total > 0 ? total : (uint64_t)-5; /* EIO */
    }
    total += (size_t)w;
    if ((size_t)w != len)
      break;
  }
  return total;
}

// ---------------------------------------------------------------------------
// sendfile
// ---------------------------------------------------------------------------

static uint64_t sys_sendfile(uint64_t out_fd, uint64_t in_fd,
                             uint64_t offset_ptr, uint64_t count, uint64_t a4,
                             uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || out_fd >= MAX_FDS || in_fd >= MAX_FDS || !t->fds[out_fd] ||
      !t->fds[in_fd])
    return (uint64_t)-9;

  vfs_node_t *out_node = t->fds[out_fd];
  vfs_node_t *in_node = t->fds[in_fd];

  uint32_t offset;
  if (offset_ptr) {
    if (!vmm_is_user_addr_range_valid(offset_ptr, sizeof(uint64_t)))
      return (uint64_t)-14;
    offset = (uint32_t)(*(uint64_t *)offset_ptr);
  } else {
    offset = t->fd_offsets[in_fd];
  }

  uint8_t *buffer = kmalloc(4096);
  if (!buffer)
    return (uint64_t)-12;

  uint32_t total_sent = 0;
  while (total_sent < count) {
    uint32_t to_read =
        (count - total_sent > 4096) ? 4096 : (uint32_t)(count - total_sent);
    int32_t bytes_read =
        (int32_t)vfs_read(in_node, offset + total_sent, to_read, buffer);
    if (bytes_read <= 0)
      break;
    int32_t bytes_written = (int32_t)vfs_write(out_node, t->fd_offsets[out_fd],
                                               (uint32_t)bytes_read, buffer);
    if (bytes_written <= 0)
      break;
    t->fd_offsets[out_fd] += (uint32_t)bytes_written;
    total_sent += (uint32_t)bytes_written;
    if (bytes_written < bytes_read)
      break;
  }

  kfree(buffer);
  if (offset_ptr)
    *(uint64_t *)offset_ptr = (uint64_t)(offset + total_sent);
  else
    t->fd_offsets[in_fd] = offset + total_sent;

  return (uint64_t)total_sent;
}

// ---------------------------------------------------------------------------
// lseek
// ---------------------------------------------------------------------------

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;

  vfs_node_t *node = t->fds[fd];
  int64_t new_offset = 0;
  if (whence == 0)
    new_offset = (int64_t)offset;
  else if (whence == 1)
    new_offset = (int64_t)t->fd_offsets[fd] + (int64_t)offset;
  else if (whence == 2)
    new_offset = (int64_t)node->length + (int64_t)offset;
  else
    return (uint64_t)-22;

  if (new_offset < 0)
    return (uint64_t)-22;
  t->fd_offsets[fd] = (uint32_t)new_offset;
  return (uint64_t)new_offset;
}

// ---------------------------------------------------------------------------
// fcntl
// ---------------------------------------------------------------------------

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
    int newfd = alloc_fd_from(t, (int)arg);
    if (newfd < 0)
      return (uint64_t)-24;
    t->fds[newfd] = t->fds[fd];
    t->fd_offsets[newfd] = t->fd_offsets[fd];
    t->fd_flags[newfd] = t->fd_flags[fd] & ~(uint64_t)FD_FLAGS_CLOEXEC_BIT;
    if (cmd == F_DUPFD_CLOEXEC)
      t->fd_flags[newfd] |= FD_FLAGS_CLOEXEC_BIT;
    fd_path_dup(t, newfd, (int)fd);
    vfs_open(t->fds[newfd]);
    return (uint64_t)newfd;
  }
  case F_GETFD: {
    uint64_t dflags = 0;
    if (t->fd_flags[fd] & FD_FLAGS_CLOEXEC_BIT)
      dflags |= 1;
    return dflags;
  }
  case F_SETFD: {
    if (arg & 1)
      t->fd_flags[fd] |= FD_FLAGS_CLOEXEC_BIT;
    else
      t->fd_flags[fd] &= ~(uint64_t)FD_FLAGS_CLOEXEC_BIT;
    return 0;
  }
  case F_GETFL: {
    uint64_t fl = t->fd_flags[fd] & (O_ACCMODE | O_APPEND | O_NONBLOCK);
    vfs_node_t *node = t->fds[fd];
    if (node && (node->flags & FS_TYPE_MASK) == FS_SOCKET) {
      socket_t *sock = (socket_t *)node->device;
      if (sock && (sock->flags & SOCK_NONBLOCK))
        fl |= O_NONBLOCK;
    }
    if (node && (node->flags & FS_NONBLOCK))
      fl |= O_NONBLOCK;
    return fl;
  }
  case F_SETFL: {
    uint64_t status_flags = arg & (O_APPEND | O_NONBLOCK);
    t->fd_flags[fd] =
        (t->fd_flags[fd] & ~(O_APPEND | O_NONBLOCK)) | status_flags;
    vfs_node_t *node = t->fds[fd];
    if (node && (node->flags & FS_TYPE_MASK) == FS_SOCKET) {
      socket_t *sock = (socket_t *)node->device;
      if (sock) {
        if (arg & O_NONBLOCK)
          sock->flags |= SOCK_NONBLOCK;
        else
          sock->flags &= ~SOCK_NONBLOCK;
      }
    }
    if (node) {
      if (arg & O_NONBLOCK)
        node->flags |= FS_NONBLOCK;
      else
        node->flags &= ~FS_NONBLOCK;
    }
    return 0;
  }
  case F_SETOWN:
    (void)arg;
    return 0;
  case F_GETLK:
  case F_SETLK:
  case F_SETLKW:
    return 0;
  case 1033: { // F_ADD_SEALS
    vfs_node_t *node = t->fds[fd];
    if ((node->flags & FS_TYPE_MASK) != FS_FILE)
      return (uint64_t)-22;
    node->impl |= (uint32_t)arg;
    return 0;
  }
  case 1034: { // F_GET_SEALS
    vfs_node_t *node = t->fds[fd];
    if ((node->flags & FS_TYPE_MASK) != FS_FILE)
      return (uint64_t)-22;
    return (uint64_t)node->impl;
  }
  default:
    klog_puts("[SYSCALL] sys_fcntl: unhandled cmd=");
    klog_uint64(cmd);
    klog_puts("\n");
    return (uint64_t)-22;
  }
}

// ---------------------------------------------------------------------------
// ftruncate / fallocate / flock / fsync / fadvise64
// ---------------------------------------------------------------------------

static uint64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;
  vfs_node_t *node = t->fds[fd];
  if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE)
    return (uint64_t)-1;
  return vfs_truncate(node, (uint32_t)length) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_fallocate(uint64_t fd, uint64_t mode, uint64_t offset,
                              uint64_t len, uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;
  vfs_node_t *node = t->fds[fd];
  if (!node)
    return (uint64_t)-9;
  if ((node->flags & FS_TYPE_MASK) != FS_FILE)
    return (uint64_t)-22;

  klog_puts("[SYSCALL] fallocate: fd=");
  klog_uint64(fd);
  klog_puts(" mode=");
  klog_uint64(mode);
  klog_puts(" offset=");
  klog_uint64(offset);
  klog_puts(" len=");
  klog_uint64(len);
  klog_puts("\n");

  int ret = vfs_fallocate(node, (int)mode, (uint32_t)offset, (uint32_t)len);
  klog_puts("[SYSCALL] fallocate: result=");
  klog_uint64((uint64_t)(int64_t)ret);
  klog_puts("\n");
  if (ret != 0)
    return ret == -1 ? (uint64_t)-1 : (uint64_t)ret;
  return 0;
}

static uint64_t sys_flock(uint64_t fd, uint64_t operation, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)fd;
  (void)operation;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0;
}

static uint64_t sys_fsync(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9;
  vfs_cache_sync(t->fds[fd]);
  return 0;
}

static uint64_t sys_fdatasync(uint64_t fd, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  return sys_fsync(fd, a1, a2, a3, a4, a5);
}

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

static uint64_t sys_mount(uint64_t source_ptr, uint64_t target_ptr,
                          uint64_t fstype_ptr, uint64_t flags,
                          uint64_t data_ptr, uint64_t a5) {
    (void)fstype_ptr;
    (void)flags;
    (void)data_ptr;
    (void)a5;

    const char *source = (const char *)source_ptr;
    const char *target = (const char *)target_ptr;

    if (!source || !target)
        return (uint64_t)-14;

    vfs_node_t *mountpoint = vfs_resolve_path(target);
    if (!mountpoint)
        return (uint64_t)-2;
    if ((mountpoint->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20;

    const char *dev_name = source;
    if (strncmp(source, "/dev/", 5) == 0)
        dev_name = source + 5;

    struct block_device *dev = NULL;
    int n = block_count();
    for (int i = 0; i < n; i++) {
        struct block_device *d = block_get(i);
        if (d && strcmp(d->name, dev_name) == 0) {
            dev = d;
            break;
        }
    }

    if (!dev)
        return (uint64_t)-6;

    if (ext4_mount(dev, mountpoint) == 0)
        return 0;
    if (ext2_mount(dev, mountpoint) == 0)
        return 0;

    return (uint64_t)-22;
}

void syscall_register_fd(void) {
  syscall_register(SYS_READ, sys_read);
  syscall_register(SYS_WRITE, sys_write);
  syscall_register(SYS_PREAD64, sys_pread64);
  syscall_register(SYS_PWRITE64, sys_pwrite64);
  syscall_register(SYS_READV, sys_readv);
  syscall_register(SYS_WRITEV, sys_writev);
  syscall_register(SYS_OPEN, sys_open);
  syscall_register(SYS_OPENAT, sys_openat);
  syscall_register(SYS_CLOSE, sys_close);
  syscall_register(SYS_CLOSE_RANGE, sys_close_range);
  syscall_register(SYS_DUP, sys_dup);
  syscall_register(SYS_DUP2, sys_dup2);
  syscall_register(SYS_LSEEK, sys_lseek);
  syscall_register(SYS_FCNTL, sys_fcntl);
  syscall_register(SYS_FTRUNCATE, sys_ftruncate);
  syscall_register(SYS_FALLOCATE, sys_fallocate);
  syscall_register(SYS_FLOCK, sys_flock);
  syscall_register(SYS_FSYNC, sys_fsync);
  syscall_register(SYS_FDATASYNC, sys_fdatasync);
  syscall_register(SYS_MOUNT, sys_mount);
  syscall_register(SYS_SENDFILE, sys_sendfile);
  syscall_register(SYS_FADVISE64, sys_fadvise64);
}
