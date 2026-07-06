// sys_fs.c — Filesystem namespace syscalls:
//   mkdir, mkdirat, unlink, unlinkat, rmdir, rename, symlink, readlink,
//   link, chmod, chown, fchmod, fchmodat, fchownat, access, faccessat2,
//   getcwd, chdir, fchdir, utimensat, futimesat, utimes, readlinkat
#include "sys_io_shared.h"
#include "../console/klog.h"
#include "../fb/framebuffer.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../socket/af_unix.h"
#include "syscall.h"
#include <stdint.h>

#define AT_REMOVEDIR 0x200

// ---------------------------------------------------------------------------
// vfs_resolve_symlink_node — resolve WITHOUT following the final symlink
// ---------------------------------------------------------------------------

vfs_node_t *vfs_resolve_symlink_node(vfs_node_t *base, const char *path) {
    if (!path || !path[0]) return NULL;

    const char *last_slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/') last_slash = p;

    vfs_node_t *parent;
    const char *last_comp;

    if (!last_slash) {
        parent    = base ? base : fs_root;
        last_comp = path;
    } else if (last_slash == path) {
        parent    = fs_root;
        last_comp = last_slash + 1;
    } else {
        size_t parent_len = (size_t)(last_slash - path);
        char parent_path[512];
        if (parent_len >= sizeof(parent_path)) return NULL;
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        parent = vfs_resolve_path_at(base ? base : fs_root, parent_path);
        if (!parent) return NULL;
        last_comp = last_slash + 1;
    }

    if (!last_comp || !last_comp[0]) return parent;
    return vfs_finddir(parent, (char *)last_comp);
}

// ---------------------------------------------------------------------------
// resolve_parent_and_name helper
// ---------------------------------------------------------------------------

vfs_node_t *resolve_parent_and_name(const char *path, char *name_out,
                                     size_t name_size) {
    if (!path) return NULL;
    size_t len = strlen(path);
    if (len == 0 || len >= 256) return NULL;

    struct thread *t = sched_get_current();
    vfs_node_t *base  = (path[0] == '/') ? fs_root
                       : (t->cwd_node ? t->cwd_node : fs_root);

    const char *last_slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/') last_slash = p;

    vfs_node_t *parent;
    const char *basename;

    if (last_slash) {
        size_t parent_len = (size_t)(last_slash - path);
        if (parent_len == 0) {
            parent = fs_root;
        } else {
            char parent_path[256];
            if (parent_len >= sizeof(parent_path)) return NULL;
            memcpy(parent_path, path, parent_len);
            parent_path[parent_len] = '\0';
            parent = vfs_resolve_path_at(base, parent_path);
        }
        basename = last_slash + 1;
    } else {
        parent   = base;
        basename = path;
    }

    if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return NULL;

    size_t blen = strlen(basename);
    if (blen == 0 || blen >= name_size) return NULL;
    strcpy(name_out, basename);
    return parent;
}

// ---------------------------------------------------------------------------
// mkdir / mkdirat
// ---------------------------------------------------------------------------

static uint64_t sys_mkdir(uint64_t pathname, uint64_t mode, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname;
    if (!path) return (uint64_t)-14;

    char clean_path[256];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(clean_path)) return (uint64_t)-14;
    strcpy(clean_path, path);
    while (len > 1 && clean_path[len - 1] == '/') { clean_path[--len] = '\0'; }
    if (strcmp(clean_path, "/") == 0) return 0;

    struct thread *t = sched_get_current();
    vfs_node_t *base  = (clean_path[0] == '/') ? fs_root
                       : (t->cwd_node ? t->cwd_node : fs_root);

    klog_puts("[MKDIR] path="); klog_puts(clean_path);
    klog_puts(" mode="); klog_uint64(mode); klog_puts("\n");

    char parent_path[256], dir_name[128];
    const char *slash = 0;
    for (const char *p = clean_path; *p; p++)
        if (*p == '/') slash = p;

    vfs_node_t *parent = base;
    if (slash) {
        size_t parent_len = (size_t)(slash - clean_path);
        if (parent_len == 0) {
            parent = fs_root;
        } else {
            if (parent_len >= sizeof(parent_path)) return (uint64_t)-14;
            for (size_t i = 0; i < parent_len; i++)
                parent_path[i] = clean_path[i];
            parent_path[parent_len] = '\0';
            parent = vfs_resolve_path_at(base, parent_path);
            if (!parent) {
                klog_puts("[MKDIR] Parent not found, creating recursively\n");
                sys_mkdir((uint64_t)parent_path, mode, 0, 0, 0, 0);
                parent = vfs_resolve_path_at(base, parent_path);
            }
        }
        size_t dlen = strlen(slash + 1);
        if (dlen == 0 || dlen >= sizeof(dir_name)) return (uint64_t)-22;
        strcpy(dir_name, slash + 1);
    } else {
        strcpy(dir_name, clean_path);
    }

    if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20;
    if (!vfs_access(parent, 3)) return (uint64_t)-13;
    mode &= ~t->umask;
    if (parent->mask & 02000) mode |= 02000;

    if (vfs_mkdir(parent, dir_name, (uint16_t)mode) != 0) {
        vfs_node_t *existing = vfs_finddir(parent, dir_name);
        if (existing && (existing->flags & FS_TYPE_MASK) == FS_DIRECTORY)
            return 0;
        return (uint64_t)-17;
    }
    vfs_node_t *created = vfs_finddir(parent, dir_name);
    if (created) vfs_chown(created, t->fsuid,
        (parent->mask & 02000) ? parent->gid : t->fsgid);
    return 0;
}

static uint64_t sys_mkdirat(uint64_t dirfd, uint64_t pathname, uint64_t mode,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname;
    if (!path) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    vfs_node_t *base_dir = fs_root;
    if (path[0] != '/') {
        if ((int64_t)dirfd == AT_FDCWD) {
            base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
            if (!base_dir) base_dir = fs_root;
        } else if (dirfd < MAX_FDS && t->fds[dirfd]) {
            base_dir = t->fds[dirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return (uint64_t)-20;
        } else {
            return (uint64_t)-9;
        }
    }

    char parent_path[128], dir_name[128];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(dir_name)) return (uint64_t)-14;

    const char *slash = 0;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p;

    vfs_node_t *parent = base_dir;
    if (slash) {
        size_t parent_len = (size_t)(slash - path);
        if (parent_len == 0) {
            parent = fs_root;
        } else {
            if (parent_len >= sizeof(parent_path)) return (uint64_t)-14;
            for (size_t i = 0; i < parent_len; i++)
                parent_path[i] = path[i];
            parent_path[parent_len] = '\0';
            parent = vfs_resolve_path_at(base_dir, parent_path);
        }
        size_t dlen = strlen(slash + 1);
        if (dlen == 0 || dlen >= sizeof(dir_name)) return (uint64_t)-22;
        strcpy(dir_name, slash + 1);
    } else {
        strcpy(dir_name, path);
    }

    if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-20;
    if (!vfs_access(parent, 3)) return (uint64_t)-13;
    mode &= ~t->umask;
    if (parent->mask & 02000) mode |= 02000;
    if (vfs_mkdir(parent, dir_name, (uint16_t)mode) != 0)
        return (uint64_t)-17;
    vfs_node_t *created = vfs_finddir(parent, dir_name);
    if (created) vfs_chown(created, t->fsuid,
        (parent->mask & 02000) ? parent->gid : t->fsgid);
    return 0;
}

// ---------------------------------------------------------------------------
// unlink / unlinkat / rmdir
// ---------------------------------------------------------------------------

static uint64_t sys_unlinkat(uint64_t dirfd, uint64_t pathname, uint64_t flags,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname;
    if (!path) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;

    vfs_node_t *base_dir = fs_root;
    if (path[0] != '/') {
        if ((int64_t)dirfd == AT_FDCWD) {
            base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
            if (!base_dir) base_dir = fs_root;
        } else if (dirfd < MAX_FDS && t->fds[dirfd]) {
            base_dir = t->fds[dirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return (uint64_t)-20;
        } else {
            return (uint64_t)-9;
        }
    }

    char parent_path[128], file_name[128];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(file_name)) return (uint64_t)-14;

    const char *slash = 0;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p;

    vfs_node_t *parent = base_dir;
    if (slash) {
        size_t parent_len = (size_t)(slash - path);
        if (parent_len == 0) {
            parent = fs_root;
        } else {
            if (parent_len >= sizeof(parent_path)) return (uint64_t)-14;
            for (size_t i = 0; i < parent_len; i++)
                parent_path[i] = path[i];
            parent_path[parent_len] = '\0';
            parent = vfs_resolve_path_at(base_dir, parent_path);
        }
        size_t flen = strlen(slash + 1);
        if (flen == 0 || flen >= sizeof(file_name)) return (uint64_t)-22;
        strcpy(file_name, slash + 1);
    } else {
        strcpy(file_name, path);
    }

    if (!parent || (parent->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return (uint64_t)-2;
    vfs_node_t *victim = vfs_finddir(parent, file_name);
    if (!victim) return (uint64_t)-2;
    if (!vfs_may_remove(parent, victim)) return (uint64_t)-13;

    // Build full path for socket unbinding
    char full_path[256];
    if (path[0] == '/') {
        strncpy(full_path, path, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
    } else {
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

    unix_unbind_by_path(full_path);
    int result = (flags & AT_REMOVEDIR) ? vfs_rmdir(parent, file_name)
                                        : vfs_unlink(parent, file_name);
    return result == 0 ? 0 : (uint64_t)-2;
}

static uint64_t sys_unlink(uint64_t pathname_ptr, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;

    char name[128];
    vfs_node_t *parent = resolve_parent_and_name(path, name, sizeof(name));
    if (!parent) return (uint64_t)-2;

    vfs_node_t *target = vfs_finddir(parent, name);
    if (!target) return (uint64_t)-2;
    if ((target->flags & FS_TYPE_MASK) == FS_DIRECTORY) return (uint64_t)-21;
    if (!vfs_may_remove(parent, target)) return (uint64_t)-13;

    struct thread *t = sched_get_current();
    if (t) {
        for (int i = 0; i < MAX_FDS; i++) {
            if (t->fds[i] == target) {
                t->fds[i] = NULL;
                t->fd_offsets[i] = 0;
            }
        }
    }
    unix_unbind_by_path(path);
    return vfs_unlink(parent, name) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_rmdir(uint64_t pathname_ptr, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;

    char name[128];
    vfs_node_t *parent = resolve_parent_and_name(path, name, sizeof(name));
    if (!parent) return (uint64_t)-2;

    vfs_node_t *target = vfs_finddir(parent, name);
    if (!target) return (uint64_t)-2;
    if ((target->flags & FS_TYPE_MASK) != FS_DIRECTORY) return (uint64_t)-20;
    if (!vfs_may_remove(parent, target)) return (uint64_t)-13;
    return vfs_rmdir(parent, name) == 0 ? 0 : (uint64_t)-1;
}

// ---------------------------------------------------------------------------
// rename
// ---------------------------------------------------------------------------

static uint64_t sys_rename(uint64_t oldpath_ptr, uint64_t newpath_ptr,
                            uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *oldpath = (const char *)oldpath_ptr;
    const char *newpath = (const char *)newpath_ptr;
    if (!oldpath || !newpath) return (uint64_t)-14;

    char old_name[128], new_name[128];
    vfs_node_t *old_parent = resolve_parent_and_name(oldpath, old_name, sizeof(old_name));
    vfs_node_t *new_parent = resolve_parent_and_name(newpath, new_name, sizeof(new_name));
    if (!old_parent) return (uint64_t)-2;
    if (!new_parent) return (uint64_t)-2;

    vfs_node_t *old_node = vfs_finddir(old_parent, old_name);
    if (!old_node) return (uint64_t)-2;
    if (!vfs_may_remove(old_parent, old_node) || !vfs_access(new_parent, 3))
        return (uint64_t)-13;

    if (old_parent == new_parent || old_parent->inode == new_parent->inode) {
        return vfs_rename(old_parent, old_name, new_name) == 0 ? 0 : (uint64_t)-1;
    }
    return (uint64_t)-18; // EXDEV
}

// ---------------------------------------------------------------------------
// symlink / readlink / readlinkat
// ---------------------------------------------------------------------------

static uint64_t sys_symlink(uint64_t target_ptr, uint64_t linkpath_ptr,
                             uint64_t a2, uint64_t a3, uint64_t a4,
                             uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *target   = (const char *)target_ptr;
    const char *linkpath = (const char *)linkpath_ptr;
    if (!target || !linkpath) return (uint64_t)-14;

    char link_name[128];
    vfs_node_t *parent = resolve_parent_and_name(linkpath, link_name, sizeof(link_name));
    if (!parent) return (uint64_t)-2;
    if (!vfs_access(parent, 3)) return (uint64_t)-13;
    if (vfs_finddir(parent, link_name)) return (uint64_t)-17;

    char target_buf[256];
    size_t t_len = strlen(target);
    if (t_len == 0 || t_len >= sizeof(target_buf)) return (uint64_t)-14;
    strcpy(target_buf, target);

    return vfs_symlink(parent, link_name, target_buf) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_readlink(uint64_t pathname_ptr, uint64_t buf_ptr,
                              uint64_t bufsiz, uint64_t a3, uint64_t a4,
                              uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    char *buf        = (char *)buf_ptr;
    if (!path || !buf) return (uint64_t)-14;
    if (bufsiz == 0)   return (uint64_t)-22;

    if (strcmp(path, "/proc/self/exe") == 0) {
        const char *exe_path = "/init";
        size_t len = strlen(exe_path);
        if (len > bufsiz) len = bufsiz;
        memcpy(buf, exe_path, len);
        return len;
    }

    struct thread *t = sched_get_current();
    vfs_node_t *base  = fs_root;
    if (t && path[0] != '/' && t->cwd_path[0]) {
        vfs_node_t *cwd = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (cwd) base = cwd;
    }

    vfs_node_t *node = vfs_resolve_symlink_node(base, path);
    if (!node) return (uint64_t)-2;
    if ((node->flags & FS_TYPE_MASK) != FS_SYMLINK) return (uint64_t)-22;

    int ret = vfs_readlink(node, buf, (uint32_t)bufsiz);
    if (ret < 0) return (uint64_t)-22;

    klog_puts("[READLINK] "); klog_puts(path); klog_puts(" -> ");
    char log_tmp[256];
    size_t log_len = (size_t)ret < 255 ? (size_t)ret : 255;
    memcpy(log_tmp, buf, log_len);
    log_tmp[log_len] = '\0';
    klog_puts(log_tmp); klog_puts("\n");
    return (uint64_t)ret;
}

static uint64_t sys_readlinkat(uint64_t dirfd, uint64_t pathname_ptr,
                                uint64_t buf_ptr, uint64_t bufsiz,
                                uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    // For now, ignore dirfd and delegate to sys_readlink
    (void)dirfd;
    return sys_readlink(pathname_ptr, buf_ptr, bufsiz, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// link
// ---------------------------------------------------------------------------

static uint64_t sys_link(uint64_t oldpath_ptr, uint64_t newpath_ptr,
                          uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *oldpath = (const char *)oldpath_ptr;
    const char *newpath = (const char *)newpath_ptr;
    if (!oldpath || !newpath) return (uint64_t)-14;

    vfs_node_t *src = vfs_resolve_path(oldpath);
    if (!src) return (uint64_t)-2;
    if ((src->flags & FS_TYPE_MASK) != FS_FILE) return (uint64_t)-1;

    char parent_path[128], file_name[128];
    size_t len = strlen(newpath);
    if (len == 0 || len >= sizeof(file_name)) return (uint64_t)-36;

    const char *slash = 0;
    for (const char *p = newpath; *p; p++)
        if (*p == '/') slash = p;

    vfs_node_t *parent = fs_root;
    if (slash) {
        size_t parent_len = (size_t)(slash - newpath);
        if (parent_len > 0) {
            if (parent_len >= sizeof(parent_path)) return (uint64_t)-36;
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
    if (!vfs_access(parent, 3)) return (uint64_t)-13;
    if (vfs_finddir(parent, file_name)) return (uint64_t)-17;
    if (vfs_create(parent, file_name, src->mask & 0777) != 0)
        return (uint64_t)-1;

    vfs_node_t *dst = vfs_finddir(parent, file_name);
    if (dst && src->length > 0) {
        uint8_t buf[512];
        uint32_t offset = 0;
        while (offset < src->length) {
            uint32_t chunk = src->length - offset;
            if (chunk > sizeof(buf)) chunk = sizeof(buf);
            uint32_t rd = vfs_read(src, offset, chunk, buf);
            if (rd == 0) break;
            vfs_write(dst, offset, rd, buf);
            offset += rd;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// chmod / chown / fchmod / fchmodat / fchownat
// ---------------------------------------------------------------------------

static uint64_t sys_chmod(uint64_t pathname_ptr, uint64_t mode, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;
    struct thread *t = sched_get_current();
    vfs_node_t *base  = fs_root;
    if (t && path[0] != '/' && t->cwd_path[0]) {
        base = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base) base = fs_root;
    }
    vfs_node_t *node = vfs_resolve_path_at(base, path);
    if (!node) return (uint64_t)-2;
    if (t && t->euid != 0 && t->fsuid != node->uid) return (uint64_t)-1;
    return vfs_chmod(node, (uint16_t)mode) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_chown(uint64_t pathname_ptr, uint64_t owner,
                           uint64_t group, uint64_t a3, uint64_t a4,
                           uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;
    struct thread *t = sched_get_current();
    vfs_node_t *base  = fs_root;
    if (t && path[0] != '/' && t->cwd_path[0]) {
        base = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base) base = fs_root;
    }
    vfs_node_t *node = vfs_resolve_path_at(base, path);
    if (!node) return (uint64_t)-2;
    uint32_t uid = (owner == (uint64_t)-1) ? node->uid : (uint32_t)owner;
    uint32_t gid = (group == (uint64_t)-1) ? node->gid : (uint32_t)group;
    if (t && t->euid != 0) {
        if (t->fsuid != node->uid || uid != node->uid || !vfs_in_group(gid))
            return (uint64_t)-1;
    }
    if (uid != node->uid || gid != node->gid) node->mask &= ~06000;
    return vfs_chown(node, uid, gid) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_fchown(uint64_t fd, uint64_t owner, uint64_t group,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;
    vfs_node_t *node = t->fds[fd];
    uint32_t uid = (owner == (uint64_t)-1) ? node->uid : (uint32_t)owner;
    uint32_t gid = (group == (uint64_t)-1) ? node->gid : (uint32_t)group;
    if (t->euid != 0 &&
        (t->fsuid != node->uid || uid != node->uid || !vfs_in_group(gid)))
        return (uint64_t)-1;
    if (uid != node->uid || gid != node->gid)
        vfs_chmod(node, (uint16_t)(node->mask & ~06000));
    return vfs_chown(node, uid, gid) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_lchown(uint64_t pathname_ptr, uint64_t owner,
                            uint64_t group, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
    return sys_chown(pathname_ptr, owner, group, a3, a4, a5);
}

static uint64_t sys_fchmod(uint64_t fd, uint64_t mode, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;
    if (t->euid != 0 && t->fsuid != t->fds[fd]->uid) return (uint64_t)-1;
    if (vfs_chmod(t->fds[fd], (uint16_t)mode) != 0) return (uint64_t)-1;
    return 0;
}

static uint64_t sys_fchmodat(uint64_t dirfd, uint64_t pathname_ptr,
                              uint64_t mode, uint64_t flags, uint64_t a4,
                              uint64_t a5) {
    (void)flags; (void)a4; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;
    struct thread *t = sched_get_current();
    vfs_node_t *base  = fs_root;
    if (path[0] != '/') {
        if ((int)dirfd == AT_FDCWD) {
            if (t && t->cwd_path[0]) {
                base = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!base) base = fs_root;
            }
        } else {
            if (dirfd >= MAX_FDS || !t->fds[dirfd]) return (uint64_t)-9;
            base = t->fds[dirfd];
        }
    }
    vfs_node_t *node = vfs_resolve_path_at(base, path);
    if (!node) return (uint64_t)-2;
    if (t && t->euid != 0 && t->fsuid != node->uid) return (uint64_t)-1;
    return vfs_chmod(node, (uint16_t)mode) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_fchownat(uint64_t dirfd, uint64_t pathname_ptr,
                              uint64_t owner, uint64_t group, uint64_t flags,
                              uint64_t a5) {
    (void)flags; (void)a5;
    const char *path = (const char *)pathname_ptr;
    if (!path) return (uint64_t)-14;
    struct thread *t = sched_get_current();
    vfs_node_t *base  = fs_root;
    if (path[0] != '/') {
        if ((int)dirfd == AT_FDCWD) {
            if (t && t->cwd_path[0]) {
                base = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!base) base = fs_root;
            }
        } else {
            if (dirfd >= MAX_FDS || !t->fds[dirfd]) return (uint64_t)-9;
            base = t->fds[dirfd];
        }
    }
    vfs_node_t *node = vfs_resolve_path_at(base, path);
    if (!node) return (uint64_t)-2;
    uint32_t uid = (owner == (uint64_t)-1) ? node->uid : (uint32_t)owner;
    uint32_t gid = (group == (uint64_t)-1) ? node->gid : (uint32_t)group;
    if (t && t->euid != 0 &&
        (t->fsuid != node->uid || uid != node->uid || !vfs_in_group(gid)))
        return (uint64_t)-1;
    if (uid != node->uid || gid != node->gid) node->mask &= ~06000;
    return vfs_chown(node, uid, gid) == 0 ? 0 : (uint64_t)-1;
}

// ---------------------------------------------------------------------------
// access / faccessat2
// ---------------------------------------------------------------------------

static uint64_t do_sys_access(int dirfd, const char *path, uint64_t mode,
                               int flags) {
    (void)flags;
    if (!path) return (uint64_t)-14;

    struct thread *t = sched_get_current();
    vfs_node_t *node  = NULL;

    if (path[0] == '/')
        node = vfs_resolve_path_at(fs_root, path);
    else if (strncmp(path, "/dev/", 5) == 0)
        node = fb_lookup_device((char *)path + 5);

    if (!node) {
        vfs_node_t *base_dir = fs_root;
        if (dirfd == AT_FDCWD) {
            if (t && t->cwd_path[0]) {
                base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
                if (!base_dir) base_dir = fs_root;
            }
        } else {
            if (dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
                return (uint64_t)-9;
            base_dir = t->fds[dirfd];
            if ((base_dir->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return (uint64_t)-20;
        }
        node = vfs_resolve_path_at(base_dir, path);
    }
    if (!node) return (uint64_t)-2;

    if (mode & ~7) return (uint64_t)-22;
    if (mode && !vfs_access(node, (uint32_t)mode)) return (uint64_t)-13;
    return 0;
}

static uint64_t sys_access(uint64_t pathname_ptr, uint64_t mode, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    return do_sys_access(AT_FDCWD, (const char *)pathname_ptr, mode, 0);
}

static uint64_t sys_faccessat2(uint64_t dirfd, uint64_t pathname_ptr,
                                uint64_t mode, uint64_t flags, uint64_t a4,
                                uint64_t a5) {
    (void)a4; (void)a5;
    return do_sys_access((int)dirfd, (const char *)pathname_ptr, mode, (int)flags);
}

// ---------------------------------------------------------------------------
// chdir / fchdir
// ---------------------------------------------------------------------------

static uint64_t sys_fchdir(uint64_t fd, uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    struct thread *t = sched_get_current();
    if (!t) return (uint64_t)-1;
    if (fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9;
    if ((t->fds[fd]->flags & FS_TYPE_MASK) != FS_DIRECTORY) return (uint64_t)-20;
    if (!vfs_access(t->fds[fd], 1)) return (uint64_t)-13;

    if (t->fd_paths[fd][0]) {
        strncpy(t->cwd_path, t->fd_paths[fd], sizeof(t->cwd_path) - 1);
        t->cwd_path[sizeof(t->cwd_path) - 1] = '\0';
        if (t->cwd_node) vfs_close(t->cwd_node);
        t->cwd_node = t->fds[fd];
        vfs_open(t->cwd_node);
        klog_puts("[FCHDIR] Changed cwd to: "); klog_puts(t->cwd_path);
        klog_puts(" via fd="); klog_uint64(fd); klog_puts("\n");
        return 0;
    }
    return (uint64_t)-9;
}

// ---------------------------------------------------------------------------
// utimensat / futimesat / utimes stubs
// ---------------------------------------------------------------------------

static uint64_t sys_utimensat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                               uint64_t flags, uint64_t a4, uint64_t a5) {
    (void)dirfd; (void)pathname; (void)times; (void)flags; (void)a4; (void)a5;
    return 0;
}

static uint64_t sys_futimesat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)dirfd; (void)pathname; (void)times; (void)a3; (void)a4; (void)a5;
    return 0;
}

static uint64_t sys_utimes(uint64_t pathname, uint64_t times, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)pathname; (void)times; (void)a2; (void)a3; (void)a4; (void)a5;
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void syscall_register_fs(void) {
    syscall_register(SYS_MKDIR,      sys_mkdir);
    syscall_register(SYS_MKDIRAT,    sys_mkdirat);
    syscall_register(SYS_UNLINK,     sys_unlink);
    syscall_register(SYS_UNLINKAT,   sys_unlinkat);
    syscall_register(SYS_RMDIR,      sys_rmdir);
    syscall_register(SYS_RENAME,     sys_rename);
    syscall_register(SYS_SYMLINK,    sys_symlink);
    syscall_register(SYS_READLINK,   sys_readlink);
    syscall_register(SYS_READLINKAT, sys_readlinkat);
    syscall_register(SYS_LINK,       sys_link);
    syscall_register(SYS_CHMOD,      sys_chmod);
    syscall_register(SYS_CHOWN,      sys_chown);
    syscall_register(SYS_FCHOWN,     sys_fchown);
    syscall_register(SYS_LCHOWN,     sys_lchown);
    syscall_register(SYS_FCHMOD,     sys_fchmod);
    syscall_register(SYS_FCHMODAT,   sys_fchmodat);
    syscall_register(SYS_FCHOWNAT,   sys_fchownat);
    syscall_register(SYS_ACCESS,     sys_access);
    syscall_register(SYS_FACCESSAT2, sys_faccessat2);
    syscall_register(SYS_FCHDIR,     sys_fchdir);
    syscall_register(SYS_UTIMENSAT,  sys_utimensat);
    syscall_register(SYS_FUTIMESAT,  sys_futimesat);
    syscall_register(SYS_UTIMES,     sys_utimes);
}
