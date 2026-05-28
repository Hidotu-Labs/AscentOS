#include "drm.h"
#include "../../../console/klog.h"
#include "../../../fb/framebuffer.h"
#include "../../../fs/ramfs.h"
#include "../../../lib/string.h"
#include "../../../mm/heap.h"
#include "../../../mm/vmm.h"

struct drm_device global_drm_dev;

/* ── External symbols ────────────────────────────────────────────────────── */
extern struct drm_gem_object *drm_gem_object_create(struct drm_device *dev, size_t size);
extern void drm_gem_object_free(struct drm_device *dev, struct drm_gem_object *obj);
extern struct drm_gem_object *drm_gem_find_by_handle(struct drm_device *dev, uint32_t handle);
extern void drm_kms_init(struct drm_device *dev);
extern struct drm_framebuffer *drm_framebuffer_create(struct drm_device *dev,
                                                      struct drm_mode_fb_cmd *cmd);
extern void drm_framebuffer_free(struct drm_device *dev, struct drm_framebuffer *fb);
extern void epoll_notify_event(struct vfs_node *node, uint32_t events);

/* drm_prop.c */
extern int drm_ioctl_obj_getprops(struct drm_device *dev, uint64_t arg);
extern int drm_ioctl_getproperty(struct drm_device *dev, uint64_t arg);
extern int drm_ioctl_atomic(struct vfs_node *node, struct drm_file *file,
                            struct drm_device *dev, uint64_t arg);
extern struct drm_prop_blob *drm_blob_create(struct drm_device *dev,
                                             const void *data, uint32_t length);
extern struct drm_prop_blob *drm_blob_find(struct drm_device *dev, uint32_t id);
extern void drm_blob_destroy(struct drm_device *dev, uint32_t id);

/* drm_file.c */
extern struct drm_file *drm_file_alloc(struct drm_device *dev);
extern void drm_file_free(struct drm_file *file);
extern uint32_t drm_file_gem_register(struct drm_file *file, struct drm_gem_object *obj);
extern struct drm_gem_object *drm_file_gem_lookup(struct drm_file *file, uint32_t handle);
extern void drm_file_gem_release(struct drm_file *file, uint32_t handle);
extern void drm_file_send_event(struct drm_file *file, struct drm_event_vblank *ev,
                                struct vfs_node *node);
extern int drm_prime_export(struct drm_gem_object *obj);
extern struct drm_gem_object *drm_prime_import(int prime_fd);

/* drm_fb.c */
extern int drm_ioctl_addfb2(struct drm_device *dev, uint64_t arg);

/* ── Helper: get drm_file from node->device ──────────────────────────────── */
/*
 * The VFS node's device pointer is set to drm_file* on open (per-client node).
 * For the template node (before first open), device points to drm_device.
 * We distinguish by checking whether the pointer is the global device.
 */
static inline struct drm_file *node_to_file(struct vfs_node *node) {
    return (struct drm_file *)node->device;
}

static struct drm_mode_object *drm_mode_object_find(struct drm_device *dev,
                                                    uint32_t id) {
    struct drm_mode_object *obj;
    list_for_each_entry(obj, &dev->kms_objects, list) {
        if (obj->id == id) return obj;
    }
    return NULL;
}

/* ── Per-client open / close ─────────────────────────────────────────────── */

/*
 * drm_open is called by vfs_open() on the per-client clone node.
 * The clone's device pointer is already set to the drm_file by drm_dri_finddir.
 */
static void drm_open(struct vfs_node *node) {
    /* Nothing extra needed — drm_file was allocated in drm_dri_finddir */
    (void)node;
}

static void drm_close(struct vfs_node *node) {
    struct drm_file *file = node_to_file(node);
    if (file) drm_file_free(file);
    /* node itself is freed by vfs_close since it's non-persistent */
}

/* ── Main ioctl dispatcher ───────────────────────────────────────────────── */

static int drm_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
    struct drm_file   *file = node_to_file(node);
    struct drm_device *dev  = file ? file->dev : NULL;
    if (!dev) return -1;

    klog_puts("[DRM] ioctl request=0x");
    klog_hex32(request);
    klog_puts("\n");

    switch (request) {

    /* ── Version / caps ──────────────────────────────────────────────── */
    case DRM_IOCTL_VERSION: {
        struct drm_version *v = (struct drm_version *)arg;
        v->version_major = 1; v->version_minor = 0; v->version_patchlevel = 0;
        if (v->name) strncpy(v->name, "AscentOS DRM", v->name_len);
        if (v->date) strncpy(v->date, "20260528", v->date_len);
        if (v->desc) strncpy(v->desc, "AscentOS Graphics Subsystem", v->desc_len);
        return 0;
    }
    case DRM_IOCTL_GET_CAP: {
        struct drm_get_cap *cap = (struct drm_get_cap *)arg;
        switch (cap->capability) {
        case 1:   cap->value = 1; break; /* DUMB_BUFFER */
        case 2:   cap->value = 1; break; /* VBLANK_HIGH_CRTC */
        case 4:   cap->value = 1; break; /* DRM_PRIME */
        case 5:   cap->value = 1; break; /* TIMESTAMP_MONOTONIC */
        case 6:   cap->value = 1; break; /* ASYNC_PAGE_FLIP */
        case 7:   cap->value = 1; break; /* ADDFB2_MODIFIERS */
        case 0x8: cap->value = 64; break; /* CURSOR_WIDTH */
        case 0x9: cap->value = 64; break; /* CURSOR_HEIGHT */
        default:  cap->value = 0; break;
        }
        return 0;
    }
    case 0x4010640D: /* DRM_IOCTL_SET_CLIENT_CAP */ {
        struct drm_set_client_cap *cap = (struct drm_set_client_cap *)arg;
        if (cap->capability == DRM_CLIENT_CAP_UNIVERSAL_PLANES && cap->value)
            file->client_caps |= (1 << 1);
        else if (cap->capability == DRM_CLIENT_CAP_ATOMIC && cap->value)
            file->client_caps |= (1 << 2);
        return 0;
    }
    case DRM_IOCTL_SET_MASTER:
        file->is_master = 1;
        return 0;
    case DRM_IOCTL_DROP_MASTER:
        file->is_master = 0;
        return 0;

    /* ── Per-client GEM ──────────────────────────────────────────────── */
    case DRM_IOCTL_GEM_CREATE: {
        struct drm_gem_create *c = (struct drm_gem_create *)arg;
        struct drm_gem_object *obj = drm_gem_object_create(dev, c->size);
        if (!obj) return -1;
        /* Register in global list AND per-client table */
        uint32_t local_h = drm_file_gem_register(file, obj);
        if (!local_h) { drm_gem_object_free(dev, obj); return -1; }
        c->handle = local_h;
        return 0;
    }
    case DRM_IOCTL_GEM_FREE: {
        struct drm_gem_free *f = (struct drm_gem_free *)arg;
        drm_file_gem_release(file, f->handle);
        return 0;
    }

    /* ── Dumb buffers (use per-client handle table) ───────────────────── */
    case DRM_IOCTL_MODE_CREATE_DUMB: {
        struct drm_mode_create_dumb *c = (struct drm_mode_create_dumb *)arg;
        c->pitch = (c->width * (c->bpp / 8) + 63) & ~63;
        c->size  = (uint64_t)c->pitch * c->height;
        struct drm_gem_object *obj = drm_gem_object_create(dev, c->size);
        if (!obj) return -1;
        uint32_t local_h = drm_file_gem_register(file, obj);
        if (!local_h) { drm_gem_object_free(dev, obj); return -1; }
        c->handle = local_h;
        return 0;
    }
    case DRM_IOCTL_MODE_MAP_DUMB: {
        struct drm_mode_map_dumb *m = (struct drm_mode_map_dumb *)arg;
        struct drm_gem_object *obj = drm_file_gem_lookup(file, m->handle);
        if (!obj) return -1;
        m->offset = obj->phys_addr | 0x1000000000000000ULL;
        return 0;
    }

    /* ── KMS resource queries ────────────────────────────────────────── */
    case DRM_IOCTL_MODE_GETRESOURCES: {
        struct drm_mode_card_res *res = (struct drm_mode_card_res *)arg;
        uint32_t fbs=0, crtcs=0, connectors=0, encoders=0;
        struct drm_mode_object *mobj;
        spinlock_acquire(&dev->lock);
        list_for_each_entry(mobj, &dev->kms_objects, list) {
            if (mobj->type == DRM_MODE_OBJECT_CRTC) {
                if (res->crtc_id_ptr && crtcs < res->count_crtcs)
                    ((uint32_t *)res->crtc_id_ptr)[crtcs] = mobj->id;
                crtcs++;
            } else if (mobj->type == DRM_MODE_OBJECT_CONNECTOR) {
                if (res->connector_id_ptr && connectors < res->count_connectors)
                    ((uint32_t *)res->connector_id_ptr)[connectors] = mobj->id;
                connectors++;
            } else if (mobj->type == DRM_MODE_OBJECT_ENCODER) {
                if (res->encoder_id_ptr && encoders < res->count_encoders)
                    ((uint32_t *)res->encoder_id_ptr)[encoders] = mobj->id;
                encoders++;
            } else if (mobj->type == DRM_MODE_OBJECT_FB) {
                if (res->fb_id_ptr && fbs < res->count_fbs)
                    ((uint32_t *)res->fb_id_ptr)[fbs] = mobj->id;
                fbs++;
            }
        }
        spinlock_release(&dev->lock);
        res->count_fbs=fbs; res->count_crtcs=crtcs;
        res->count_connectors=connectors; res->count_encoders=encoders;
        res->min_width=0; res->max_width=4096;
        res->min_height=0; res->max_height=4096;
        return 0;
    }
    case DRM_IOCTL_MODE_GETPLANERESOURCES: {
        struct { uint64_t plane_id_ptr; uint32_t count_planes; } *res = (void *)arg;
        uint32_t planes = 0;
        struct drm_mode_object *mobj;
        spinlock_acquire(&dev->lock);
        list_for_each_entry(mobj, &dev->kms_objects, list) {
            if (mobj->type == DRM_MODE_OBJECT_PLANE) {
                if (res->plane_id_ptr && planes < res->count_planes)
                    ((uint32_t *)res->plane_id_ptr)[planes] = mobj->id;
                planes++;
            }
        }
        spinlock_release(&dev->lock);
        res->count_planes = planes;
        return 0;
    }
    case 0xC02064B6: { /* DRM_IOCTL_MODE_GETPLANE */
        struct { uint32_t plane_id, crtc_id, fb_id, possible_crtcs,
                          gamma_size, count_formats; uint64_t format_type_ptr; } *p = (void *)arg;
        spinlock_acquire(&dev->lock);
        struct drm_mode_object *mobj = drm_mode_object_find(dev, p->plane_id);
        if (!mobj || mobj->type != DRM_MODE_OBJECT_PLANE) {
            spinlock_release(&dev->lock); return -1;
        }
        struct drm_plane *plane = (struct drm_plane *)mobj;
        p->crtc_id=0; p->fb_id=0; p->possible_crtcs=plane->possible_crtcs;
        p->gamma_size=0; p->count_formats=1;
        if (p->format_type_ptr) ((uint32_t *)p->format_type_ptr)[0] = 0x34325258;
        spinlock_release(&dev->lock);
        return 0;
    }
    case DRM_IOCTL_MODE_GETCRTC: {
        struct drm_mode_get_crtc *c = (struct drm_mode_get_crtc *)arg;
        spinlock_acquire(&dev->lock);
        struct drm_mode_object *mobj = drm_mode_object_find(dev, c->crtc_id);
        if (!mobj || mobj->type != DRM_MODE_OBJECT_CRTC) {
            spinlock_release(&dev->lock); return -1;
        }
        struct drm_crtc *crtc = (struct drm_crtc *)mobj;
        c->fb_id = crtc->fb ? crtc->fb->base.id : 0;
        c->x=0; c->y=0; c->gamma_size=0; c->mode_valid=1;
        c->mode.clock=60000; c->mode.hdisplay=fb_get_width();
        c->mode.vdisplay=fb_get_height(); c->mode.vrefresh=60;
        strcpy(c->mode.name, "Native");
        spinlock_release(&dev->lock);
        return 0;
    }

    case DRM_IOCTL_MODE_GETENCODER: {
        struct drm_mode_get_encoder *e = (struct drm_mode_get_encoder *)arg;
        spinlock_acquire(&dev->lock);
        struct drm_mode_object *mobj = drm_mode_object_find(dev, e->encoder_id);
        if (!mobj || mobj->type != DRM_MODE_OBJECT_ENCODER) {
            spinlock_release(&dev->lock); return -1;
        }
        struct drm_encoder *enc = (struct drm_encoder *)mobj;
        e->encoder_type=enc->encoder_type; e->possible_crtcs=enc->possible_crtcs;
        e->possible_clones=0;
        struct drm_mode_object *obj;
        list_for_each_entry(obj, &dev->kms_objects, list) {
            if (obj->type == DRM_MODE_OBJECT_CRTC) { e->crtc_id=obj->id; break; }
        }
        spinlock_release(&dev->lock);
        return 0;
    }
    case DRM_IOCTL_MODE_GETCONNECTOR: {
        struct drm_mode_get_connector *c = (struct drm_mode_get_connector *)arg;
        spinlock_acquire(&dev->lock);
        struct drm_mode_object *mobj = drm_mode_object_find(dev, c->connector_id);
        if (!mobj || mobj->type != DRM_MODE_OBJECT_CONNECTOR) {
            spinlock_release(&dev->lock); return -1;
        }
        struct drm_connector *conn = (struct drm_connector *)mobj;
        c->connector_type=conn->connector_type; c->connector_type_id=1;
        c->connection=conn->connection_status;
        c->mm_width=300; c->mm_height=200; c->subpixel=1;
        if (conn->encoder) c->encoder_id=conn->encoder->base.id;

        /* Expose connector properties — wlroots needs CRTC_ID on the connector */
        uint32_t prop_count = mobj->prop_count;
        if (c->props_ptr && c->count_props >= prop_count) {
            uint32_t *prop_ids  = (uint32_t *)c->props_ptr;
            uint64_t *prop_vals = (uint64_t *)c->prop_values_ptr;
            for (uint32_t i = 0; i < prop_count; i++) {
                prop_ids[i]  = mobj->props[i].prop_id;
                prop_vals[i] = mobj->props[i].value;
            }
        }
        c->count_props = prop_count;

        c->count_modes=1;
        if (c->modes_ptr) {
            struct drm_mode_modeinfo *m = (struct drm_mode_modeinfo *)c->modes_ptr;
            m->clock=60000; m->hdisplay=fb_get_width();
            m->vdisplay=fb_get_height(); m->vrefresh=60;
            m->flags = 0; m->type = 0x48; /* DRM_MODE_TYPE_DRIVER | PREFERRED */
            strcpy(m->name, "Native");
        }
        c->count_encoders=0;
        if (conn->encoder) {
            c->count_encoders=1;
            if (c->encoders_ptr) ((uint32_t *)c->encoders_ptr)[0]=conn->encoder->base.id;
        }
        spinlock_release(&dev->lock);
        return 0;
    }

    /* ── Framebuffer management ──────────────────────────────────────── */
    case DRM_IOCTL_MODE_ADDFB: {
        struct drm_mode_fb_cmd *cmd = (struct drm_mode_fb_cmd *)arg;
        /* Resolve local handle → global gem object */
        struct drm_gem_object *gem = drm_file_gem_lookup(file, cmd->handle);
        if (!gem) return -1;
        /* Temporarily patch handle to global for drm_framebuffer_create */
        uint32_t saved = cmd->handle;
        cmd->handle = gem->handle;
        struct drm_framebuffer *fb = drm_framebuffer_create(dev, cmd);
        cmd->handle = saved;
        if (!fb) return -1;
        cmd->fb_id = fb->base.id;
        return 0;
    }
    case DRM_IOCTL_MODE_RMFB: {
        uint32_t fb_id = *(uint32_t *)arg;
        spinlock_acquire(&dev->lock);
        struct drm_mode_object *mobj = drm_mode_object_find(dev, fb_id);
        if (!mobj || mobj->type != DRM_MODE_OBJECT_FB) {
            spinlock_release(&dev->lock); return -1;
        }
        spinlock_release(&dev->lock);
        drm_framebuffer_free(dev, (struct drm_framebuffer *)mobj);
        return 0;
    }
    case DRM_IOCTL_MODE_ADDFB2:
        /* Full multi-planar path — resolves handles via global gem list */
        return drm_ioctl_addfb2(dev, arg);

    /* ── Legacy modesetting ──────────────────────────────────────────── */
    case DRM_IOCTL_MODE_SETCRTC: {
        struct drm_mode_crtc *crtc_cmd = (struct drm_mode_crtc *)arg;
        spinlock_acquire(&dev->lock);
        struct drm_mode_object *crtc_obj = drm_mode_object_find(dev, crtc_cmd->crtc_id);
        struct drm_mode_object *fb_obj   = drm_mode_object_find(dev, crtc_cmd->fb_id);
        if (!crtc_obj || crtc_obj->type != DRM_MODE_OBJECT_CRTC) {
            spinlock_release(&dev->lock); return -1;
        }
        struct drm_crtc *crtc = (struct drm_crtc *)crtc_obj;
        if (fb_obj && fb_obj->type == DRM_MODE_OBJECT_FB)
            crtc->fb = (struct drm_framebuffer *)fb_obj;
        spinlock_release(&dev->lock);
        return 0;
    }
    case DRM_IOCTL_MODE_PAGE_FLIP: {
        struct drm_mode_crtc_page_flip *flip = (struct drm_mode_crtc_page_flip *)arg;
        spinlock_acquire(&dev->lock);
        struct drm_mode_object *crtc_obj = drm_mode_object_find(dev, flip->crtc_id);
        struct drm_mode_object *fb_obj   = drm_mode_object_find(dev, flip->fb_id);
        if (!crtc_obj || crtc_obj->type != DRM_MODE_OBJECT_CRTC ||
            !fb_obj   || fb_obj->type   != DRM_MODE_OBJECT_FB) {
            spinlock_release(&dev->lock); return -1;
        }
        struct drm_crtc *crtc = (struct drm_crtc *)crtc_obj;
        crtc->fb = (struct drm_framebuffer *)fb_obj;
        if (flip->flags & DRM_MODE_PAGE_FLIP_EVENT) {
            struct drm_event_vblank ev = {0};
            ev.base.type   = DRM_EVENT_FLIP_COMPLETE;
            ev.base.length = sizeof(ev);
            ev.user_data   = flip->user_data;
            spinlock_release(&dev->lock);
            drm_file_send_event(file, &ev, node);
            return 0;
        }
        spinlock_release(&dev->lock);
        return 0;
    }

    /* ── Atomic modesetting ──────────────────────────────────────────── */
    case DRM_IOCTL_MODE_ATOMIC:
        return drm_ioctl_atomic(node, file, dev, arg);
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
        return drm_ioctl_obj_getprops(dev, arg);
    case 0xC04064AA: /* DRM_IOCTL_MODE_GETPROPERTY */
        return drm_ioctl_getproperty(dev, arg);
    case DRM_IOCTL_MODE_CREATEPROPBLOB: {
        struct drm_mode_create_blob *b = (struct drm_mode_create_blob *)arg;
        if (!b->data || !b->length) return -1;
        struct drm_prop_blob *blob = drm_blob_create(dev, (void *)b->data, b->length);
        if (!blob) return -1;
        b->blob_id = blob->id;
        return 0;
    }
    case DRM_IOCTL_MODE_DESTROYPROPBLOB: {
        struct drm_mode_destroy_blob *b = (struct drm_mode_destroy_blob *)arg;
        drm_blob_destroy(dev, b->blob_id);
        return 0;
    }

    /* ── GEM PRIME / DMA-buf ─────────────────────────────────────────── */
    case DRM_IOCTL_PRIME_HANDLE_TO_FD: {
        struct drm_prime_handle *p = (struct drm_prime_handle *)arg;
        struct drm_gem_object *obj = drm_file_gem_lookup(file, p->handle);
        if (!obj) return -1;
        int prime_fd = drm_prime_export(obj);
        if (prime_fd < 0) return -1;
        p->fd = prime_fd;
        return 0;
    }
    case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
        struct drm_prime_handle *p = (struct drm_prime_handle *)arg;
        struct drm_gem_object *obj = drm_prime_import(p->fd);
        if (!obj) return -1;
        uint32_t local_h = drm_file_gem_register(file, obj);
        if (!local_h) return -1;
        p->handle = local_h;
        return 0;
    }

    default:
        return -1;
    }
}

/* ── mmap — resolve GEM offset to physical pages ─────────────────────────── */

static uint64_t drm_vfs_mmap(vfs_node_t *node, uint64_t addr, uint64_t length,
                             uint64_t prot, uint64_t flags, uint64_t offset) {
    (void)node; (void)flags; (void)prot;
    if ((offset & 0x1000000000000000ULL) == 0) return (uint64_t)-1;
    uint64_t phys = offset & ~0x1000000000000000ULL;
    uint64_t vaddr = addr;
    if (vaddr == 0) {
        extern uint64_t mm_alloc_mmap_region(uint64_t length);
        vaddr = mm_alloc_mmap_region(length);
        if (vaddr == 0) vaddr = 0x500000000000ULL + (offset & 0xFFFFFFF);
    }
    if (vaddr == 0) return (uint64_t)-1;

    uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW
                        | PAGE_FLAG_PWT | PAGE_FLAG_PAT;
    uint32_t num_pages = (length + 4095) / 4096;
    uint64_t *pml4 = vmm_get_active_pml4();
    for (uint32_t i = 0; i < num_pages; i++)
        vmm_map_page(pml4, vaddr + i*4096, phys + i*4096, page_flags);
    return vaddr;
}

/* ── poll / read — per-client event queue ────────────────────────────────── */

static int drm_poll(struct vfs_node *node, int events) {
    struct drm_file *file = node_to_file(node);
    int revents = 0x0004; /* POLLOUT always */
    if (file) {
        spinlock_acquire(&file->lock);
        if (!list_empty(&file->event_queue)) revents |= 0x0001; /* POLLIN */
        spinlock_release(&file->lock);
    }
    return revents & events;
}

static uint32_t drm_read(struct vfs_node *node, uint32_t offset,
                         uint32_t length, uint8_t *buffer) {
    struct drm_file *file = node_to_file(node);
    (void)offset;
    if (!file) return 0;

    spinlock_acquire(&file->lock);
    while (list_empty(&file->event_queue)) {
        spinlock_release(&file->lock);
        struct thread *current = sched_get_current();
        wait_queue_entry_t entry = { .thread = current, .next = NULL };
        wait_queue_add(&file->event_wq, &entry);
        current->state = THREAD_BLOCKED;
        sched_yield();
        wait_queue_remove(&file->event_wq, &entry);
        current->state = THREAD_RUNNING;
        spinlock_acquire(&file->lock);
    }

    struct drm_pending_event *e =
        list_first_entry(&file->event_queue, struct drm_pending_event, list);
    uint32_t event_size = e->event.base.length;
    if (length < event_size) { spinlock_release(&file->lock); return 0; }
    memcpy(buffer, &e->event, event_size);
    list_del(&e->list);
    spinlock_release(&file->lock);
    kfree(e);
    return event_size;
}

/* ── Per-client node factory via custom finddir ──────────────────────────── */
/*
 * When userland opens /dev/dri/card0, ramfs_finddir returns the template node.
 * We override the dri directory's finddir to return a fresh per-client clone
 * instead, so each open() gets its own vfs_node_t with device→drm_file.
 */

static vfs_node_t *drm_dri_finddir(vfs_node_t *dir, char *name) {
    (void)dir;
    if (strcmp(name, "card0") != 0) return NULL;

    /* Allocate per-client drm_file */
    struct drm_file *file = drm_file_alloc(&global_drm_dev);
    if (!file) return NULL;

    /* Allocate a fresh non-persistent clone node */
    vfs_node_t *clone = kmalloc(sizeof(vfs_node_t));
    if (!clone) { drm_file_free(file); return NULL; }
    vfs_node_init(clone);
    strcpy(clone->name, "card0");
    clone->flags    = FS_CHARDEV; /* non-persistent: freed when refcount→0 */
    clone->mask     = 0666;
    clone->device   = file;       /* ← per-client state */
    clone->ioctl    = drm_ioctl;
    clone->mmap     = drm_vfs_mmap;
    clone->open     = drm_open;
    clone->close    = drm_close;
    clone->read     = drm_read;
    clone->poll     = drm_poll;
    clone->wait_queue = &file->event_wq;
    clone->refcount = 0; /* vfs_open will bump to 1 */

    return clone;
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void drm_init(void) {
    memset(&global_drm_dev, 0, sizeof(struct drm_device));
    global_drm_dev.name          = "card0";
    global_drm_dev.next_gem_handle = 1;
    global_drm_dev.next_kms_id   = 1000;
    global_drm_dev.next_blob_id  = 1;
    global_drm_dev.next_prime_id = 1;
    spinlock_init(&global_drm_dev.lock);
    INIT_LIST_HEAD(&global_drm_dev.gem_objects);
    INIT_LIST_HEAD(&global_drm_dev.kms_objects);
    INIT_LIST_HEAD(&global_drm_dev.event_queue);
    INIT_LIST_HEAD(&global_drm_dev.blob_objects);
    INIT_LIST_HEAD(&global_drm_dev.file_list);
    wait_queue_init(&global_drm_dev.event_wq);
    drm_kms_init(&global_drm_dev);

    /* Bridge hardware framebuffer as a global GEM object */
    void *fb_base = fb_get_base();
    if (fb_base) {
        uint64_t fb_phys = vmm_virt_to_phys(vmm_get_active_pml4(), (uint64_t)fb_base);
        size_t   fb_size = fb_get_height() * fb_get_pitch();
        struct drm_gem_object *fb_obj = kmalloc(sizeof(struct drm_gem_object));
        if (fb_obj) {
            memset(fb_obj, 0, sizeof(struct drm_gem_object));
            fb_obj->size      = fb_size;
            fb_obj->phys_addr = fb_phys;
            fb_obj->virt_addr = fb_base;
            fb_obj->refcount  = 1;
            fb_obj->handle    = 0xF0B0;
            spinlock_acquire(&global_drm_dev.lock);
            list_add_tail(&fb_obj->list, &global_drm_dev.gem_objects);
            spinlock_release(&global_drm_dev.lock);
            klog_puts("[DRM] Bridged HW framebuffer to GEM handle 0xF0B0\n");
        }
    }
}

void drm_register_vfs(void) {
    vfs_node_t *dev_dir = vfs_resolve_path("/dev");
    if (!dev_dir) return;
    vfs_mkdir(dev_dir, "dri", 0755);
    vfs_node_t *dri_dir = vfs_resolve_path("/dev/dri");
    if (!dri_dir) return;

    /* Override finddir on the dri directory to return per-client clones */
    dri_dir->finddir = drm_dri_finddir;

    klog_puts("[DRM] Registered /dev/dri/card0 (per-client mode)\n");
}
