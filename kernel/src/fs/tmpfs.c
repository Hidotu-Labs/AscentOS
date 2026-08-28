#include "tmpfs.h"
#include "vfs.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "arch/uaccess.h"

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))
#define PAGE_SIZE 4096

static uint32_t tmpfs_next_inode = 1;

static tmpfs_sb_t *tmpfs_sb_from_node(vfs_node_t *node) {
    if (!node || !node->device)
        return NULL;
    uint32_t type = node->flags & FS_TYPE_MASK;
    if (type == FS_FILE)
        return ((tmpfs_file_t *)node->device)->sb;
    if (type == FS_DIRECTORY)
        return ((tmpfs_dir_t *)node->device)->sb;
    if (type == FS_SYMLINK)
        return ((tmpfs_symlink_t *)node->device)->sb;
    return NULL;
}

static uint32_t tmpfs_alloc_inode(tmpfs_sb_t *sb) {
    spinlock_acquire(&sb->lock);
    if (sb->used_inodes >= sb->max_inodes) {
        spinlock_release(&sb->lock);
        return 0;
    }
    sb->used_inodes++;
    spinlock_release(&sb->lock);
    return __atomic_fetch_add(&tmpfs_next_inode, 1, __ATOMIC_RELAXED);
}

static void tmpfs_free_inode(tmpfs_sb_t *sb) {
    spinlock_acquire(&sb->lock);
    if (sb->used_inodes > 0)
        sb->used_inodes--;
    spinlock_release(&sb->lock);
}

static bool tmpfs_charge_bytes(tmpfs_sb_t *sb, uint64_t bytes) {
    spinlock_acquire(&sb->lock);
    if (sb->used_bytes + bytes > sb->max_bytes) {
        spinlock_release(&sb->lock);
        return false;
    }
    sb->used_bytes += bytes;
    spinlock_release(&sb->lock);
    return true;
}

static void tmpfs_uncharge_bytes(tmpfs_sb_t *sb, uint64_t bytes) {
    spinlock_acquire(&sb->lock);
    if (sb->used_bytes >= bytes)
        sb->used_bytes -= bytes;
    else
        sb->used_bytes = 0;
    spinlock_release(&sb->lock);
}

static tmpfs_page_t *tmpfs_get_page(tmpfs_file_t *file, uint32_t page_index) {
    tmpfs_page_t *p = file->pages;
    while (p) {
        if (p->page_index == page_index)
            return p;
        p = p->next;
    }
    return NULL;
}

static tmpfs_page_t *tmpfs_get_or_alloc_page(tmpfs_file_t *file,
                                               uint32_t page_index) {
    tmpfs_sb_t *sb = file->sb;

    spinlock_acquire(&file->lock);
    tmpfs_page_t *p = tmpfs_get_page(file, page_index);
    if (p) {
        spinlock_release(&file->lock);
        return p;
    }
    spinlock_release(&file->lock);

    if (!tmpfs_charge_bytes(sb, PAGE_SIZE))
        return NULL;

    void *frame = pmm_alloc_page();
    if (!frame) {
        tmpfs_uncharge_bytes(sb, PAGE_SIZE);
        return NULL;
    }
    memset(PHYS_TO_VIRT(frame), 0, PAGE_SIZE);

    tmpfs_page_t *np = kmalloc(sizeof(tmpfs_page_t));
    if (!np) {
        pmm_free_page(frame);
        tmpfs_uncharge_bytes(sb, PAGE_SIZE);
        return NULL;
    }
    np->page_index = page_index;
    np->phys       = (uint64_t)frame;
    np->next       = NULL;

    spinlock_acquire(&file->lock);
    tmpfs_page_t *existing = tmpfs_get_page(file, page_index);
    if (existing) {
        spinlock_release(&file->lock);
        pmm_free_page(frame);
        tmpfs_uncharge_bytes(sb, PAGE_SIZE);
        kfree(np);
        return existing;
    }
    np->next    = file->pages;
    file->pages = np;
    file->num_pages++;
    spinlock_release(&file->lock);
    return np;
}

static void tmpfs_free_all_pages(tmpfs_file_t *file) {
    tmpfs_sb_t *sb = file->sb;

    spinlock_acquire(&file->lock);
    tmpfs_page_t *p = file->pages;
    file->pages     = NULL;
    uint32_t count  = file->num_pages;
    file->num_pages = 0;
    spinlock_release(&file->lock);

    while (p) {
        tmpfs_page_t *next = p->next;
        pmm_free_page((void *)p->phys);
        kfree(p);
        p = next;
    }
    tmpfs_uncharge_bytes(sb, (uint64_t)count * PAGE_SIZE);
}

static uint32_t tmpfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                            uint8_t *buffer) {
    if (!node || !node->device || !buffer)
        return 0;
    if (offset >= node->length)
        return 0;
    if (offset + size > node->length)
        size = node->length - offset;
    if (size == 0)
        return 0;

    tmpfs_file_t *file = (tmpfs_file_t *)node->device;
    uint32_t      done = 0;

    while (done < size) {
        uint32_t page_index = (offset + done) / PAGE_SIZE;
        uint32_t page_off   = (offset + done) % PAGE_SIZE;
        uint32_t chunk      = PAGE_SIZE - page_off;
        if (chunk > size - done)
            chunk = size - done;

        spinlock_acquire(&file->lock);
        tmpfs_page_t *pg = tmpfs_get_page(file, page_index);
        if (!pg) {
            spinlock_release(&file->lock);
            if (is_user_ptr((uint64_t)buffer))
                clear_user(buffer + done, chunk);
            else
                memset(buffer + done, 0, chunk);
        } else {
            uint8_t *virt = (uint8_t *)PHYS_TO_VIRT(pg->phys);
            if (is_user_ptr((uint64_t)buffer)) {
                unsigned long uncopied = copy_to_user(buffer + done, virt + page_off, chunk);
                spinlock_release(&file->lock);
                if (uncopied > 0) {
                    done += (chunk - (uint32_t)uncopied);
                    break;
                }
            } else {
                memcpy(buffer + done, virt + page_off, chunk);
                spinlock_release(&file->lock);
            }
        }
        done += chunk;
    }
    return done;
}

static uint32_t tmpfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                             uint8_t *buffer) {
    if (!node || !node->device || !buffer || size == 0)
        return 0;

    tmpfs_file_t *file = (tmpfs_file_t *)node->device;

    uint32_t done = 0;
    while (done < size) {
        uint32_t page_index = (offset + done) / PAGE_SIZE;
        uint32_t page_off   = (offset + done) % PAGE_SIZE;
        uint32_t chunk      = PAGE_SIZE - page_off;
        if (chunk > size - done)
            chunk = size - done;

        tmpfs_page_t *pg = tmpfs_get_or_alloc_page(file, page_index);
        if (!pg)
            break;

        uint8_t *virt = (uint8_t *)PHYS_TO_VIRT(pg->phys);
        if (is_user_ptr((uint64_t)buffer)) {
            unsigned long uncopied = copy_from_user(virt + page_off, buffer + done, chunk);
            if (uncopied > 0) {
                done += (chunk - (uint32_t)uncopied);
                break;
            }
        } else {
            memcpy(virt + page_off, buffer + done, chunk);
        }
        done += chunk;
    }

    if (offset + done > node->length)
        node->length = offset + done;

    return done;
}

static int tmpfs_truncate(vfs_node_t *node, uint32_t new_len) {
    if (!node || (node->flags & FS_TYPE_MASK) != FS_FILE || !node->device)
        return -1;

    tmpfs_file_t *file = (tmpfs_file_t *)node->device;
    uint32_t      old_len = node->length;

    if (new_len == 0) {
        tmpfs_free_all_pages(file);
        node->length = 0;
        return 0;
    }

    if (new_len > old_len) {
        uint32_t first_new_page = old_len / PAGE_SIZE;
        uint32_t last_new_page  = (new_len - 1) / PAGE_SIZE;
        for (uint32_t i = first_new_page; i <= last_new_page; i++) {
            if (!tmpfs_get_or_alloc_page(file, i))
                return -1;
        }
        uint32_t zero_start = old_len % PAGE_SIZE;
        if (zero_start != 0) {
            spinlock_acquire(&file->lock);
            tmpfs_page_t *pg = tmpfs_get_page(file, first_new_page);
            if (pg) {
                uint8_t *virt     = (uint8_t *)PHYS_TO_VIRT(pg->phys);
                uint32_t zero_len = PAGE_SIZE - zero_start;
                memset(virt + zero_start, 0, zero_len);
            }
            spinlock_release(&file->lock);
        }
    } else {
        uint32_t first_free = (new_len + PAGE_SIZE - 1) / PAGE_SIZE;
        spinlock_acquire(&file->lock);
        tmpfs_page_t *prev = NULL;
        tmpfs_page_t *p    = file->pages;
        while (p) {
            if (p->page_index >= first_free) {
                tmpfs_page_t *next = p->next;
                if (prev)
                    prev->next = next;
                else
                    file->pages = next;
                pmm_free_page((void *)p->phys);
                kfree(p);
                file->num_pages--;
                tmpfs_uncharge_bytes(file->sb, PAGE_SIZE);
                p = next;
            } else {
                prev = p;
                p    = p->next;
            }
        }
        if (new_len % PAGE_SIZE != 0) {
            tmpfs_page_t *pg = tmpfs_get_page(file, new_len / PAGE_SIZE);
            if (pg) {
                uint8_t *virt = (uint8_t *)PHYS_TO_VIRT(pg->phys);
                uint32_t tail = new_len % PAGE_SIZE;
                memset(virt + tail, 0, PAGE_SIZE - tail);
            }
        }
        spinlock_release(&file->lock);
    }

    node->length = new_len;
    return 0;
}

static int tmpfs_statfs(vfs_node_t *node, struct statfs_buf *buf) {
    if (!node || !buf)
        return -1;
    tmpfs_sb_t *sb = tmpfs_sb_from_node(node);
    if (!sb)
        return -1;

    spinlock_acquire(&sb->lock);
    uint64_t total_pages = sb->max_bytes / PAGE_SIZE;
    uint64_t used_pages  = (sb->used_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t free_pages  = total_pages > used_pages ? total_pages - used_pages : 0;
    uint64_t free_inodes = sb->max_inodes > sb->used_inodes
                               ? sb->max_inodes - sb->used_inodes : 0;
    spinlock_release(&sb->lock);

    buf->f_type    = TMPFS_MAGIC;
    buf->f_bsize   = PAGE_SIZE;
    buf->f_blocks  = (int64_t)total_pages;
    buf->f_bfree   = (int64_t)free_pages;
    buf->f_bavail  = (int64_t)free_pages;
    buf->f_files   = (int64_t)sb->max_inodes;
    buf->f_ffree   = (int64_t)free_inodes;
    buf->f_fsid[0] = 0;
    buf->f_fsid[1] = 0;
    buf->f_namelen = 255;
    buf->f_frsize  = PAGE_SIZE;
    buf->f_flags   = 0;
    return 0;
}

static struct dirent *tmpfs_readdir(vfs_node_t *node, uint32_t index) {
    if (!node || !node->device)
        return NULL;

    static struct dirent d;
    memset(&d, 0, sizeof(struct dirent));

    if (index == 0) {
        strcpy(d.name, ".");
        d.ino = node->inode;
        return &d;
    }
    if (index == 1) {
        strcpy(d.name, "..");
        d.ino = node->inode;
        return &d;
    }

    index -= 2;
    tmpfs_dir_t   *dir  = (tmpfs_dir_t *)node->device;
    tmpfs_child_t *curr = dir->children;
    for (uint32_t i = 0; i < index && curr; i++)
        curr = curr->next;

    if (curr) {
        strncpy(d.name, curr->node->name, 127);
        d.name[127] = '\0';
        d.ino = curr->node->inode;
        return &d;
    }
    return NULL;
}

static vfs_node_t *tmpfs_finddir(vfs_node_t *node, char *name) {
    if (!node || !node->device)
        return NULL;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return node;

    tmpfs_dir_t   *dir  = (tmpfs_dir_t *)node->device;
    tmpfs_child_t *curr = dir->children;
    while (curr) {
        if (strcmp(curr->node->name, name) == 0)
            return curr->node;
        curr = curr->next;
    }
    return NULL;
}

static void tmpfs_add_child(vfs_node_t *parent, vfs_node_t *child) {
    tmpfs_dir_t   *dir = (tmpfs_dir_t *)parent->device;
    tmpfs_child_t *cn  = kmalloc(sizeof(tmpfs_child_t));
    if (!cn)
        return;
    cn->node      = child;
    cn->next      = dir->children;
    dir->children = cn;
}

static int tmpfs_chmod(vfs_node_t *node, uint16_t permission) {
    if (!node)
        return -1;
    node->mask = permission & 0x0FFF;
    return 0;
}

static int tmpfs_chown(vfs_node_t *node, uint32_t uid, uint32_t gid) {
    if (!node)
        return -1;
    node->uid = uid;
    node->gid = gid;
    return 0;
}

static int tmpfs_create(vfs_node_t *node, char *name, uint16_t permission);
static int tmpfs_mkdir(vfs_node_t *node, char *name, uint16_t permission);
static int tmpfs_unlink(vfs_node_t *node, char *name);
static int tmpfs_rmdir(vfs_node_t *node, char *name);
static int tmpfs_rename(vfs_node_t *node, char *old_name, char *new_name);
static int tmpfs_symlink(vfs_node_t *node, char *name, char *target);
static int tmpfs_readlink(vfs_node_t *node, char *buf, uint32_t size);
static int tmpfs_mknod(vfs_node_t *node, char *name, uint16_t permission,
                        uint32_t flags, void *device);

static void tmpfs_wire_dir_ops(vfs_node_t *n) {
    n->create  = tmpfs_create;
    n->mkdir   = tmpfs_mkdir;
    n->unlink  = tmpfs_unlink;
    n->rmdir   = tmpfs_rmdir;
    n->rename  = tmpfs_rename;
    n->symlink = tmpfs_symlink;
    n->mknod   = tmpfs_mknod;
}

static vfs_node_t *tmpfs_make_node(tmpfs_sb_t *sb, const char *name,
                                    uint16_t perm, uint32_t type) {
    uint32_t ino = tmpfs_alloc_inode(sb);
    if (!ino)
        return NULL;

    vfs_node_t *n = kmalloc(sizeof(vfs_node_t));
    if (!n) {
        tmpfs_free_inode(sb);
        return NULL;
    }
    vfs_node_init(n);
    strncpy(n->name, name, 127);
    n->name[127] = '\0';
    n->mask      = perm;
    n->uid       = 0;
    n->gid       = 0;
    n->flags     = type | FS_PERSISTENT;
    n->inode     = ino;
    n->length    = 0;
    n->impl      = 0;
    n->ptr       = NULL;
    n->chmod     = tmpfs_chmod;
    n->chown     = tmpfs_chown;
    n->statfs    = tmpfs_statfs;

    if (type == FS_FILE) {
        tmpfs_file_t *f = kmalloc(sizeof(tmpfs_file_t));
        if (!f) {
            kfree(n);
            tmpfs_free_inode(sb);
            return NULL;
        }
        f->pages     = NULL;
        f->num_pages = 0;
        f->sb        = sb;
        spinlock_init(&f->lock);
        n->device   = f;
        n->read     = tmpfs_read;
        n->write    = tmpfs_write;
        n->truncate = tmpfs_truncate;
    } else if (type == FS_DIRECTORY) {
        tmpfs_dir_t *d = kmalloc(sizeof(tmpfs_dir_t));
        if (!d) {
            kfree(n);
            tmpfs_free_inode(sb);
            return NULL;
        }
        d->children = NULL;
        d->sb       = sb;
        n->device   = d;
        n->readdir  = tmpfs_readdir;
        n->finddir  = tmpfs_finddir;
    }

    return n;
}

static int tmpfs_create(vfs_node_t *node, char *name, uint16_t permission) {
    if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return -1;
    if (tmpfs_finddir(node, name))
        return -1;

    tmpfs_sb_t *sb    = ((tmpfs_dir_t *)node->device)->sb;
    vfs_node_t *child = tmpfs_make_node(sb, name, permission, FS_FILE);
    if (!child)
        return -1;

    tmpfs_add_child(node, child);
    return 0;
}

static int tmpfs_mkdir(vfs_node_t *node, char *name, uint16_t permission) {
    if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return -1;
    if (tmpfs_finddir(node, name))
        return -1;

    tmpfs_sb_t *sb    = ((tmpfs_dir_t *)node->device)->sb;
    vfs_node_t *child = tmpfs_make_node(sb, name, permission, FS_DIRECTORY);
    if (!child)
        return -1;

    tmpfs_wire_dir_ops(child);
    tmpfs_add_child(node, child);
    return 0;
}

static int tmpfs_symlink(vfs_node_t *node, char *name, char *target) {
    if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return -1;
    if (tmpfs_finddir(node, name))
        return -1;

    tmpfs_sb_t *sb  = ((tmpfs_dir_t *)node->device)->sb;
    uint32_t    ino = tmpfs_alloc_inode(sb);
    if (!ino)
        return -1;

    vfs_node_t *child = kmalloc(sizeof(vfs_node_t));
    if (!child) {
        tmpfs_free_inode(sb);
        return -1;
    }
    vfs_node_init(child);
    strncpy(child->name, name, 127);
    child->name[127] = '\0';
    child->mask      = 0777;
    child->flags     = FS_SYMLINK | FS_PERSISTENT;
    child->inode     = ino;
    child->impl      = 0;
    child->chmod     = tmpfs_chmod;
    child->chown     = tmpfs_chown;
    child->statfs    = tmpfs_statfs;
    child->readlink  = tmpfs_readlink;

    tmpfs_symlink_t *sl = kmalloc(sizeof(tmpfs_symlink_t));
    if (!sl) {
        kfree(child);
        tmpfs_free_inode(sb);
        return -1;
    }
    strncpy(sl->target, target, 511);
    sl->target[511] = '\0';
    sl->sb          = sb;
    child->device   = sl;
    child->length   = (uint32_t)strlen(sl->target);

    tmpfs_add_child(node, child);
    return 0;
}

static int tmpfs_readlink(vfs_node_t *node, char *buf, uint32_t size) {
    if (!node || !node->device || !buf || size == 0)
        return -1;
    tmpfs_symlink_t *sl  = (tmpfs_symlink_t *)node->device;
    uint32_t         len = (uint32_t)strlen(sl->target);
    if (len > size)
        len = size;
    memcpy(buf, sl->target, len);
    return (int)len;
}

static int tmpfs_mknod(vfs_node_t *node, char *name, uint16_t permission,
                        uint32_t flags, void *device) {
    if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
        return -1;
    if (tmpfs_finddir(node, name))
        return -1;

    tmpfs_sb_t *sb  = ((tmpfs_dir_t *)node->device)->sb;
    uint32_t    ino = tmpfs_alloc_inode(sb);
    if (!ino)
        return -1;

    vfs_node_t *child = kmalloc(sizeof(vfs_node_t));
    if (!child) {
        tmpfs_free_inode(sb);
        return -1;
    }
    vfs_node_init(child);
    strncpy(child->name, name, 127);
    child->name[127] = '\0';
    child->mask      = permission & 0x0FFF;
    child->flags     = (flags & FS_TYPE_MASK) | FS_PERSISTENT;
    child->inode     = ino;
    child->impl      = 0;
    child->device    = device;
    child->chmod     = tmpfs_chmod;
    child->chown     = tmpfs_chown;
    child->statfs    = tmpfs_statfs;

    tmpfs_add_child(node, child);
    return 0;
}

static int tmpfs_unlink(vfs_node_t *node, char *name) {
    if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY || !node->device)
        return -1;

    tmpfs_dir_t   *dir  = (tmpfs_dir_t *)node->device;
    tmpfs_child_t *prev = NULL;
    tmpfs_child_t *curr = dir->children;

    while (curr) {
        if (strcmp(curr->node->name, name) == 0) {
            uint32_t type = curr->node->flags & FS_TYPE_MASK;
            if (type == FS_DIRECTORY)
                return -1;

            if (prev)
                prev->next = curr->next;
            else
                dir->children = curr->next;

            if (type == FS_FILE && curr->node->device) {
                tmpfs_file_t *f = (tmpfs_file_t *)curr->node->device;
                tmpfs_sb_t   *sb = f->sb;
                tmpfs_free_all_pages(f);
                kfree(f);
                if (sb)
                    tmpfs_free_inode(sb);
            } else if (type == FS_SYMLINK && curr->node->device) {
                tmpfs_symlink_t *sl = (tmpfs_symlink_t *)curr->node->device;
                tmpfs_sb_t      *sb = sl->sb;
                kfree(sl);
                if (sb)
                    tmpfs_free_inode(sb);
            }

            kfree(curr->node);
            kfree(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    return -1;
}

static int tmpfs_rmdir(vfs_node_t *node, char *name) {
    if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY || !node->device)
        return -1;

    tmpfs_dir_t   *dir  = (tmpfs_dir_t *)node->device;
    tmpfs_child_t *prev = NULL;
    tmpfs_child_t *curr = dir->children;

    while (curr) {
        if (strcmp(curr->node->name, name) == 0) {
            if ((curr->node->flags & FS_TYPE_MASK) != FS_DIRECTORY)
                return -1;

            tmpfs_dir_t *child_dir = (tmpfs_dir_t *)curr->node->device;
            if (!child_dir || child_dir->children)
                return -1;

            if (prev)
                prev->next = curr->next;
            else
                dir->children = curr->next;

            tmpfs_sb_t *sb = child_dir->sb;
            kfree(child_dir);
            kfree(curr->node);
            kfree(curr);
            if (sb)
                tmpfs_free_inode(sb);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    return -1;
}

static int tmpfs_rename(vfs_node_t *node, char *old_name, char *new_name) {
    if (!node || (node->flags & FS_TYPE_MASK) != FS_DIRECTORY || !node->device)
        return -1;

    vfs_node_t *existing = tmpfs_finddir(node, new_name);
    if (existing) {
        if ((existing->flags & FS_TYPE_MASK) == FS_DIRECTORY)
            return -1;
        tmpfs_unlink(node, new_name);
    }

    vfs_node_t *child = tmpfs_finddir(node, old_name);
    if (!child)
        return -1;

    strncpy(child->name, new_name, 127);
    child->name[127] = '\0';
    return 0;
}

vfs_node_t *tmpfs_create_root(tmpfs_sb_t *sb) {
    vfs_node_t *root = tmpfs_make_node(sb, "tmp", 0777, FS_DIRECTORY);
    if (!root)
        return NULL;
    tmpfs_wire_dir_ops(root);
    return root;
}

void tmpfs_mount_at_sized(const char *path, uint64_t max_bytes,
                           uint64_t max_inodes) {
    tmpfs_sb_t *sb = kmalloc(sizeof(tmpfs_sb_t));
    if (!sb)
        return;

    sb->max_bytes   = max_bytes  ? max_bytes  : TMPFS_DEFAULT_MAX_BYTES;
    sb->max_inodes  = max_inodes ? max_inodes : TMPFS_DEFAULT_MAX_INODES;
    sb->used_bytes  = 0;
    sb->used_inodes = 0;
    spinlock_init(&sb->lock);

    vfs_node_t *mountpoint = vfs_resolve_path(path);
    if (!mountpoint) {
        if (fs_root && fs_root->mkdir) {
            const char *base = path;
            for (const char *p = path; *p; p++)
                if (*p == '/')
                    base = p + 1;
            fs_root->mkdir(fs_root, (char *)base, 0777);
            mountpoint = vfs_resolve_path(path);
        }
    }

    if (!mountpoint) {
        kfree(sb);
        klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                               " tmpfs: mountpoint not found: ");
        klog_puts(path);
        klog_puts("\n");
        return;
    }

    vfs_node_t *root = tmpfs_create_root(sb);
    if (!root) {
        kfree(sb);
        klog_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET
                               " tmpfs: failed to create root node\n");
        return;
    }

    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            name = p + 1;
    strncpy(root->name, name, 127);
    root->name[127] = '\0';

    vfs_mount_ex(mountpoint, root, "tmpfs", "tmpfs");

    klog_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " tmpfs mounted at ");
    klog_puts(path);
    klog_puts(" (max=");
    klog_uint64(sb->max_bytes / (1024 * 1024));
    klog_puts("MB inodes=");
    klog_uint64(sb->max_inodes);
    klog_puts(")\n");
}

void tmpfs_mount_at(const char *path) {
    tmpfs_mount_at_sized(path, TMPFS_DEFAULT_MAX_BYTES,
                         TMPFS_DEFAULT_MAX_INODES);
}
