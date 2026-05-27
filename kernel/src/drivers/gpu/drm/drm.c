#include "drm.h"
#include "../../../console/klog.h"
#include "../../../fb/framebuffer.h"
#include "../../../fs/ramfs.h"
#include "../../../lib/string.h"
#include "../../../mm/heap.h"
#include "../../../mm/vmm.h"

struct drm_device global_drm_dev;

extern struct drm_gem_object *drm_gem_object_create(struct drm_device *dev,
                                                    size_t size);
extern void drm_gem_object_free(struct drm_device *dev,
                                struct drm_gem_object *obj);
extern struct drm_gem_object *drm_gem_find_by_handle(struct drm_device *dev,
                                                     uint32_t handle);
extern void drm_kms_init(struct drm_device *dev);
extern struct drm_framebuffer *
drm_framebuffer_create(struct drm_device *dev, struct drm_mode_fb_cmd *cmd);
extern void drm_framebuffer_free(struct drm_device *dev,
                                 struct drm_framebuffer *fb);
extern void epoll_notify_event(struct vfs_node *node, uint32_t events);

static struct drm_mode_object *drm_mode_object_find(struct drm_device *dev,
                                                    uint32_t id) {
  struct drm_mode_object *obj;
  list_for_each_entry(obj, &dev->kms_objects, list) {
    if (obj->id == id)
      return obj;
  }
  return NULL;
}

static int drm_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  struct drm_device *dev = (struct drm_device *)node->device;
  if (!dev)
    return -1;

  klog_puts("[DRM] ioctl request=0x");
  klog_hex32(request);
  klog_puts("\n");

  switch (request) {
  case DRM_IOCTL_VERSION: {
    struct drm_version *v = (struct drm_version *)arg;
    v->version_major = 1;
    v->version_minor = 0;
    v->version_patchlevel = 0;
    if (v->name)
      strncpy(v->name, "AscentOS DRM", v->name_len);
    if (v->date)
      strncpy(v->date, "20260521", v->date_len);
    if (v->desc)
      strncpy(v->desc, "AscentOS Graphics Subsystem", v->desc_len);
    return 0;
  }
  case DRM_IOCTL_GET_CAP: {
    struct drm_get_cap *cap = (struct drm_get_cap *)arg;
    if (cap->capability == 1 /* DUMB_BUFFER */) {
      cap->value = 1;
    } else if (cap->capability == 0x8 /* CURSOR_WIDTH */) {
      cap->value = 64;
    } else if (cap->capability == 0x9 /* CURSOR_HEIGHT */) {
      cap->value = 64;
    } else if (cap->capability == 0x2 /* VBLANK_HIGH_CRTC */) {
      cap->value = 1;
    } else {
      cap->value = 0;
    }
    return 0;
  }
  case DRM_IOCTL_GEM_CREATE: {
    struct drm_gem_create *c = (struct drm_gem_create *)arg;
    struct drm_gem_object *obj = drm_gem_object_create(dev, c->size);
    if (!obj)
      return -1;
    c->handle = obj->handle;
    return 0;
  }
  case DRM_IOCTL_GEM_FREE: {
    struct drm_gem_free *f = (struct drm_gem_free *)arg;
    struct drm_gem_object *obj = drm_gem_find_by_handle(dev, f->handle);
    if (!obj)
      return -1;
    drm_gem_object_free(dev, obj);
    return 0;
  }
  case DRM_IOCTL_MODE_GETRESOURCES: {
    struct drm_mode_card_res *res = (struct drm_mode_card_res *)arg;
    uint32_t fbs = 0, crtcs = 0, connectors = 0, encoders = 0;

    struct drm_mode_object *mobj;
    spinlock_acquire(&dev->lock);
    list_for_each_entry(mobj, &dev->kms_objects, list) {
      if (mobj->type == DRM_MODE_OBJECT_CRTC) {
        if (res->crtc_id_ptr && crtcs < res->count_crtcs) {
          ((uint32_t *)res->crtc_id_ptr)[crtcs] = mobj->id;
        }
        crtcs++;
      } else if (mobj->type == DRM_MODE_OBJECT_CONNECTOR) {
        if (res->connector_id_ptr && connectors < res->count_connectors) {
          ((uint32_t *)res->connector_id_ptr)[connectors] = mobj->id;
        }
        connectors++;
      } else if (mobj->type == DRM_MODE_OBJECT_ENCODER) {
        if (res->encoder_id_ptr && encoders < res->count_encoders) {
          ((uint32_t *)res->encoder_id_ptr)[encoders] = mobj->id;
        }
        encoders++;
      } else if (mobj->type == DRM_MODE_OBJECT_FB) {
        if (res->fb_id_ptr && fbs < res->count_fbs) {
          ((uint32_t *)res->fb_id_ptr)[fbs] = mobj->id;
        }
        fbs++;
      }
    }
    spinlock_release(&dev->lock);

    res->count_fbs = fbs;
    res->count_crtcs = crtcs;
    res->count_connectors = connectors;
    res->count_encoders = encoders;
    res->min_width = 0;
    res->max_width = 4096;
    res->min_height = 0;
    res->max_height = 4096;
    return 0;
  }
  case DRM_IOCTL_MODE_GETPLANERESOURCES: {
    struct {
      uint64_t plane_id_ptr;
      uint32_t count_planes;
    } *res = (void *)arg;

    uint32_t planes = 0;
    struct drm_mode_object *mobj;
    spinlock_acquire(&dev->lock);
    list_for_each_entry(mobj, &dev->kms_objects, list) {
      if (mobj->type == DRM_MODE_OBJECT_PLANE) {
        if (res->plane_id_ptr && planes < res->count_planes) {
          ((uint32_t *)res->plane_id_ptr)[planes] = mobj->id;
        }
        planes++;
      }
    }
    spinlock_release(&dev->lock);
    res->count_planes = planes;
    return 0;
  }
  case 0xC02064B6: { // DRM_IOCTL_MODE_GETPLANE
    struct {
      uint32_t plane_id;
      uint32_t crtc_id;
      uint32_t fb_id;
      uint32_t possible_crtcs;
      uint32_t gamma_size;
      uint32_t count_formats;
      uint64_t format_type_ptr;
    } *p = (void *)arg;

    spinlock_acquire(&dev->lock);
    struct drm_mode_object *mobj = drm_mode_object_find(dev, p->plane_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_PLANE) {
      spinlock_release(&dev->lock);
      return -1;
    }
    struct drm_plane *plane = (struct drm_plane *)mobj;
    p->crtc_id = 0; // Default none
    p->fb_id = 0;
    p->possible_crtcs = plane->possible_crtcs;
    p->gamma_size = 0;
    p->count_formats = 1;
    if (p->format_type_ptr) {
      ((uint32_t *)p->format_type_ptr)[0] = 0x34325258; // DRM_FORMAT_XRGB8888
    }
    spinlock_release(&dev->lock);
    return 0;
  }
  case DRM_IOCTL_MODE_GETCRTC: {
    struct drm_mode_get_crtc *c = (struct drm_mode_get_crtc *)arg;
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *mobj = drm_mode_object_find(dev, c->crtc_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_CRTC) {
      spinlock_release(&dev->lock);
      return -1;
    }
    struct drm_crtc *crtc = (struct drm_crtc *)mobj;
    c->fb_id = crtc->fb ? crtc->fb->base.id : 0;
    c->x = 0;
    c->y = 0;
    c->gamma_size = 0;
    c->mode_valid = 1;

    c->mode.clock = 60000;
    c->mode.hdisplay = fb_get_width();
    c->mode.vdisplay = fb_get_height();
    c->mode.vrefresh = 60;
    strcpy(c->mode.name, "Native");

    spinlock_release(&dev->lock);
    return 0;
  }
  case DRM_IOCTL_MODE_GETENCODER: {
    struct drm_mode_get_encoder *e = (struct drm_mode_get_encoder *)arg;
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *mobj = drm_mode_object_find(dev, e->encoder_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_ENCODER) {
      spinlock_release(&dev->lock);
      return -1;
    }
    struct drm_encoder *enc = (struct drm_encoder *)mobj;
    e->encoder_type = enc->encoder_type;
    e->possible_crtcs = enc->possible_crtcs;
    e->possible_clones = 0;

    struct drm_mode_object *obj;
    list_for_each_entry(obj, &dev->kms_objects, list) {
      if (obj->type == DRM_MODE_OBJECT_CRTC) {
        e->crtc_id = obj->id;
        break;
      }
    }
    spinlock_release(&dev->lock);
    return 0;
  }
  case DRM_IOCTL_MODE_GETCONNECTOR: {
    struct drm_mode_get_connector *c = (struct drm_mode_get_connector *)arg;
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *mobj = drm_mode_object_find(dev, c->connector_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_CONNECTOR) {
      spinlock_release(&dev->lock);
      return -1;
    }
    struct drm_connector *conn = (struct drm_connector *)mobj;
    c->connector_type = conn->connector_type;
    c->connector_type_id = 1;
    c->connection = conn->connection_status;
    c->mm_width = 300;
    c->mm_height = 200;
    c->subpixel = 1;

    if (conn->encoder)
      c->encoder_id = conn->encoder->base.id;

    c->count_props = 0;
    if (c->props_ptr && c->count_props > 0) {
      // Mock properties if needed, but 0 is fine for now
    }
    c->count_modes = 1;
    if (c->modes_ptr && c->count_modes > 0) {
      struct drm_mode_modeinfo *m = (struct drm_mode_modeinfo *)c->modes_ptr;
      m->clock = 60000;
      m->hdisplay = fb_get_width();
      m->vdisplay = fb_get_height();
      m->vrefresh = 60;
      strcpy(m->name, "Native");
    }

    c->count_encoders = 0;
    if (conn->encoder) {
      c->count_encoders = 1;
      if (c->encoders_ptr) {
        ((uint32_t *)c->encoders_ptr)[0] = conn->encoder->base.id;
      }
    }

    spinlock_release(&dev->lock);
    return 0;
  }
  case 0xC04064AA: { // DRM_IOCTL_MODE_GETPROPERTY
    struct {
      uint64_t values_ptr;
      uint64_t enum_blob_ptr;
      uint32_t prop_id;
      uint32_t flags;
      char name[32];
      uint32_t count_values;
      uint32_t count_enum_blobs;
    } *p = (void *)arg;
    
    p->flags = 0;
    p->count_values = 0;
    p->count_enum_blobs = 0;
    strcpy(p->name, "StubProperty");
    return 0;
  }
  case DRM_IOCTL_MODE_ADDFB: {
    struct drm_mode_fb_cmd *cmd = (struct drm_mode_fb_cmd *)arg;
    struct drm_framebuffer *fb = drm_framebuffer_create(dev, cmd);
    if (!fb)
      return -1;
    cmd->fb_id = fb->base.id;
    return 0;
  }
  case DRM_IOCTL_MODE_RMFB: {
    uint32_t fb_id = *(uint32_t *)arg;
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *mobj = drm_mode_object_find(dev, fb_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_FB) {
      spinlock_release(&dev->lock);
      return -1;
    }
    spinlock_release(&dev->lock);
    drm_framebuffer_free(dev, (struct drm_framebuffer *)mobj);
    return 0;
  }
  case DRM_IOCTL_MODE_SETCRTC: {
    struct drm_mode_crtc *crtc_cmd = (struct drm_mode_crtc *)arg;
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *crtc_obj =
        drm_mode_object_find(dev, crtc_cmd->crtc_id);
    struct drm_mode_object *fb_obj = drm_mode_object_find(dev, crtc_cmd->fb_id);

    if (!crtc_obj || crtc_obj->type != DRM_MODE_OBJECT_CRTC) {
      spinlock_release(&dev->lock);
      return -1;
    }

    struct drm_crtc *crtc = (struct drm_crtc *)crtc_obj;
    if (fb_obj && fb_obj->type == DRM_MODE_OBJECT_FB) {
      crtc->fb = (struct drm_framebuffer *)fb_obj;
      klog_puts("[DRM] SETCRTC: Attached FB to CRTC\n");
    }
    spinlock_release(&dev->lock);
    return 0;
  }
  case DRM_IOCTL_MODE_PAGE_FLIP: {
    struct drm_mode_crtc_page_flip *flip =
        (struct drm_mode_crtc_page_flip *)arg;

    spinlock_acquire(&dev->lock);
    struct drm_mode_object *crtc_obj = drm_mode_object_find(dev, flip->crtc_id);
    struct drm_mode_object *fb_obj = drm_mode_object_find(dev, flip->fb_id);
    if (!crtc_obj || crtc_obj->type != DRM_MODE_OBJECT_CRTC || !fb_obj ||
        fb_obj->type != DRM_MODE_OBJECT_FB) {
      spinlock_release(&dev->lock);
      return -1;
    }

    struct drm_crtc *crtc = (struct drm_crtc *)crtc_obj;
    crtc->fb = (struct drm_framebuffer *)fb_obj;

    if (flip->flags & DRM_MODE_PAGE_FLIP_EVENT) {
      struct drm_pending_event *e = kmalloc(sizeof(struct drm_pending_event));
      if (e) {
        memset(e, 0, sizeof(struct drm_pending_event));
        e->event.base.type = DRM_EVENT_FLIP_COMPLETE;
        e->event.base.length = sizeof(struct drm_event_vblank);
        e->event.user_data = flip->user_data;
        list_add_tail(&e->list, &dev->event_queue);
        wait_queue_wake_all(&dev->event_wq);
        epoll_notify_event(node, POLLIN);
      }
    }
    spinlock_release(&dev->lock);
    return 0;
  }
  case 0x4010640D: // DRM_IOCTL_SET_CLIENT_CAP
    return 0;      // Stub success
  case DRM_IOCTL_SET_MASTER:
  case DRM_IOCTL_DROP_MASTER:
    return 0;
  case DRM_IOCTL_MODE_CREATE_DUMB: {
    struct drm_mode_create_dumb *c = (struct drm_mode_create_dumb *)arg;
    c->pitch = (c->width * (c->bpp / 8) + 63) & ~63;
    c->size = (uint64_t)c->pitch * c->height;
    struct drm_gem_object *obj = drm_gem_object_create(dev, c->size);
    if (!obj)
      return -1;
    c->handle = obj->handle;
    return 0;
  }
  case DRM_IOCTL_MODE_MAP_DUMB: {
    struct drm_mode_map_dumb *m = (struct drm_mode_map_dumb *)arg;
    struct drm_gem_object *obj = drm_gem_find_by_handle(dev, m->handle);
    if (!obj)
      return -1;
    m->offset = obj->phys_addr | 0x1000000000000000ULL;
    return 0;
  }
  default:
    return -1;
  }
}

static uint64_t drm_vfs_mmap(vfs_node_t *node, uint64_t addr, uint64_t length,
                             uint64_t prot, uint64_t flags, uint64_t offset) {
  (void)node;
  (void)flags;
  (void)prot;
  if ((offset & 0x1000000000000000ULL) == 0)
    return (uint64_t)-1;
  uint64_t phys = offset & ~0x1000000000000000ULL;
  uint64_t vaddr = addr;
  if (vaddr == 0) {
    extern uint64_t mm_alloc_mmap_region(uint64_t length);
    vaddr = mm_alloc_mmap_region(length);
    if (vaddr == 0)
      vaddr = 0x500000000000ULL + (offset & 0xFFFFFFF);
  }
  if (vaddr == 0)
    return (uint64_t)-1;

  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW;
  page_flags |= (PAGE_FLAG_PWT | PAGE_FLAG_PAT);

  uint32_t num_pages = (length + 4095) / 4096;
  uint64_t *pml4 = vmm_get_active_pml4();
  for (uint32_t i = 0; i < num_pages; i++) {
    vmm_map_page(pml4, vaddr + i * 4096, phys + i * 4096, page_flags);
  }
  return vaddr;
}

static void drm_open(struct vfs_node *node) { (void)node; }

static void drm_close(struct vfs_node *node) { (void)node; }

static int drm_poll(struct vfs_node *node, int events) {
  struct drm_device *dev = (struct drm_device *)node->device;
  int revents = 0;

  spinlock_acquire(&dev->lock);
  if (!list_empty(&dev->event_queue)) {
    revents |= 0x0001; // POLLIN
  }
  spinlock_release(&dev->lock);
  
  // DRM devices are typically always writable for IOCTLs
  revents |= 0x0004; // POLLOUT

  return revents & events;
}

static uint32_t drm_read(struct vfs_node *node, uint32_t offset,
                         uint32_t length, uint8_t *buffer) {
  struct drm_device *dev = (struct drm_device *)node->device;
  (void)offset;

  spinlock_acquire(&dev->lock);
  while (list_empty(&dev->event_queue)) {
    // Check if we should block. For now, since we don't have per-FD flags
    // easily accessible in this simple VFS, we'll block unless we want
    // to implement a specific check.
    // However, if the caller came via epoll, they only call read when
    // data is ready.

    spinlock_release(&dev->lock);

    struct thread *current = sched_get_current();
    wait_queue_entry_t entry;
    entry.thread = current;
    entry.next = NULL;

    wait_queue_add(&dev->event_wq, &entry);
    current->state = THREAD_BLOCKED;
    sched_yield();
    wait_queue_remove(&dev->event_wq, &entry);
    current->state = THREAD_RUNNING;

    spinlock_acquire(&dev->lock);
  }

  struct drm_pending_event *e =
      list_first_entry(&dev->event_queue, struct drm_pending_event, list);
  uint32_t event_size = e->event.base.length;
  if (length < event_size) {
    spinlock_release(&dev->lock);
    return 0;
  }

  memcpy(buffer, &e->event, event_size);
  list_del(&e->list);
  spinlock_release(&dev->lock);
  kfree(e);
  return event_size;
}

void drm_init(void) {
  memset(&global_drm_dev, 0, sizeof(struct drm_device));
  global_drm_dev.name = "card0";
  global_drm_dev.next_gem_handle = 1;
  global_drm_dev.next_kms_id = 1000;
  spinlock_init(&global_drm_dev.lock);
  INIT_LIST_HEAD(&global_drm_dev.gem_objects);
  INIT_LIST_HEAD(&global_drm_dev.kms_objects);
  INIT_LIST_HEAD(&global_drm_dev.event_queue);
  wait_queue_init(&global_drm_dev.event_wq);
  drm_kms_init(&global_drm_dev);

  void *fb_base = fb_get_base();
  if (fb_base) {
    uint64_t fb_phys =
        vmm_virt_to_phys(vmm_get_active_pml4(), (uint64_t)fb_base);
    size_t fb_size = fb_get_height() * fb_get_pitch();
    struct drm_gem_object *fb_obj = kmalloc(sizeof(struct drm_gem_object));
    if (fb_obj) {
      memset(fb_obj, 0, sizeof(struct drm_gem_object));
      fb_obj->size = fb_size;
      fb_obj->phys_addr = fb_phys;
      fb_obj->virt_addr = fb_base;
      fb_obj->refcount = 1;
      fb_obj->handle = 0xF0B0;
      spinlock_acquire(&global_drm_dev.lock);
      list_add_tail(&fb_obj->list, &global_drm_dev.gem_objects);
      spinlock_release(&global_drm_dev.lock);
      klog_puts("[DRM] Bridged Hardware Framebuffer to GEM handle 0xF0B0\n");
    }
  }
}

void drm_register_vfs(void) {
  vfs_node_t *dev_dir = vfs_resolve_path("/dev");
  if (!dev_dir)
    return;
  vfs_mkdir(dev_dir, "dri", 0755);
  vfs_node_t *dri_dir = vfs_resolve_path("/dev/dri");
  if (!dri_dir)
    return;
  vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
  if (!node)
    return;
  vfs_node_init(node);
  strcpy(node->name, "card0");
  node->flags = FS_CHARDEV | FS_PERSISTENT;
  node->mask = 0666;
  node->device = &global_drm_dev;
  node->ioctl = drm_ioctl;
  node->mmap = drm_vfs_mmap;
  node->open = drm_open;
  node->close = drm_close;
  node->read = drm_read;
  node->poll = drm_poll;
  node->wait_queue = &global_drm_dev.event_wq;
  ramfs_mount_node(dri_dir, node);
  klog_puts("[DRM] Registered /dev/dri/card0\n");
}
