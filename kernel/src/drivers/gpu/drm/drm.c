#include "drm.h"
#include "../../../apic/lapic_timer.h"
#include "../../../console/klog.h"
#include "../../../fb/framebuffer.h"
#include "../../../fs/ramfs.h"
#include "../../../lib/string.h"
#include "../../../mm/heap.h"
#include "../../../mm/vmm.h"

struct drm_device global_drm_dev;

/* ── External symbols ────────────────────────────────────────────────────── */
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

static uint32_t drm_event_sequence = 1;

static void drm_fill_vblank_event(struct drm_event_vblank *ev, uint32_t crtc_id) {
  uint64_t ms = lapic_timer_get_ms();
  ev->tv_sec = (uint32_t)(ms / 1000);
  ev->tv_usec = (uint32_t)((ms % 1000) * 1000);
  ev->sequence = drm_event_sequence++;
  ev->crtc_id = crtc_id;
}

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
extern uint32_t drm_file_gem_register(struct drm_file *file,
                                      struct drm_gem_object *obj);
extern struct drm_gem_object *drm_file_gem_lookup(struct drm_file *file,
                                                  uint32_t handle);
extern void drm_file_gem_release(struct drm_file *file, uint32_t handle);
extern void drm_file_send_event(struct drm_file *file,
                                struct drm_event_vblank *ev,
                                struct vfs_node *node);
extern int drm_prime_export(struct drm_gem_object *obj);
extern struct drm_gem_object *drm_prime_import(int prime_fd);

/* drm_fb.c */
extern int drm_ioctl_addfb2(struct drm_file *file, struct drm_device *dev,
                            uint64_t arg);

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
    if (obj->id == id)
      return obj;
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
  if (file)
    drm_file_free(file);
  /* node itself is freed by vfs_close since it's non-persistent */
}

/* ── Display Commit (Software Blit) ─────────────────────────────────────── */
static void drm_commit(struct drm_device *dev) {
  if (!dev)
    return;
  spinlock_acquire(&dev->lock);
  struct drm_mode_object *obj;
  list_for_each_entry(obj, &dev->kms_objects, list) {
    if (obj->type == DRM_MODE_OBJECT_CRTC) {
      struct drm_crtc *crtc = (struct drm_crtc *)obj;
      if (crtc->fb && crtc->fb->gem_obj && crtc->fb->gem_obj->virt_addr) {
        void *hw_fb = fb_get_base();
        if (hw_fb) {
          uint32_t width = fb_get_width();
          uint32_t height = fb_get_height();
          uint32_t hw_pitch = fb_get_pitch();
          uint32_t sw_pitch = crtc->fb->pitch;

          if (hw_pitch == sw_pitch) {
            klog_puts("[DRM] Blit: fast copy, size=");
            klog_uint64((size_t)height * hw_pitch);
            klog_puts("\n");

            /* Check if the first few pixels are non-zero to see if anything is
             * being rendered */
            uint32_t *pixels = (uint32_t *)crtc->fb->gem_obj->virt_addr;
            bool all_zero = true;
            for (int i = 0; i < 16; i++) {
              if (pixels[i] != 0) {
                all_zero = false;
                break;
              }
            }
            if (all_zero) {
              klog_puts("[DRM] Warning: first 16 pixels are zero\n");
            } else {
              klog_puts("[DRM] Info: first pixels are non-zero: ");
              klog_hex32(pixels[0]);
              klog_puts("\n");
            }

            memcpy(hw_fb, crtc->fb->gem_obj->virt_addr,
                   (size_t)height * hw_pitch);
            __asm__ volatile("sfence" ::: "memory");
          } else {
            uint32_t copy_len = width * 4;
            if (copy_len > hw_pitch)
              copy_len = hw_pitch;
            if (copy_len > sw_pitch)
              copy_len = sw_pitch;

            klog_puts("[DRM] Blit: line copy (pitch mismatch), lines=");
            klog_uint64(height);
            klog_puts("\n");

            /* Check if the first few pixels are non-zero to see if anything is
             * being rendered */
            uint32_t *pixels = (uint32_t *)crtc->fb->gem_obj->virt_addr;
            bool all_zero = true;
            for (int i = 0; i < 16; i++) {
              if (pixels[i] != 0) {
                all_zero = false;
                break;
              }
            }
            if (all_zero) {
              klog_puts("[DRM] Warning: first 16 pixels are zero\n");
            } else {
              klog_puts("[DRM] Info: first pixels are non-zero: ");
              klog_hex32(pixels[0]);
              klog_puts("\n");
            }

            for (uint32_t y = 0; y < height; y++) {
              memcpy((uint8_t *)hw_fb + y * hw_pitch,
                     (uint8_t *)crtc->fb->gem_obj->virt_addr + y * sw_pitch,
                     copy_len);
            }
            __asm__ volatile("sfence" ::: "memory");
          }

          struct drm_plane *cursor = crtc->cursor;
          if (cursor && cursor->fb && cursor->fb->gem_obj &&
              cursor->fb->gem_obj->virt_addr && cursor->fb->bpp == 32) {
            int32_t cx = cursor->crtc_x;
            int32_t cy = cursor->crtc_y;
            uint32_t cw = cursor->crtc_w ? cursor->crtc_w : cursor->fb->width;
            uint32_t ch = cursor->crtc_h ? cursor->crtc_h : cursor->fb->height;
            if (cw > cursor->fb->width)
              cw = cursor->fb->width;
            if (ch > cursor->fb->height)
              ch = cursor->fb->height;

            uint8_t *src_base = (uint8_t *)cursor->fb->gem_obj->virt_addr;
            uint8_t *dst_base = (uint8_t *)hw_fb;
            for (uint32_t sy = 0; sy < ch; sy++) {
              int32_t dy = cy + (int32_t)sy;
              if (dy < 0 || dy >= (int32_t)height)
                continue;
              uint32_t *src = (uint32_t *)(src_base + sy * cursor->fb->pitch);
              uint32_t *dst = (uint32_t *)(dst_base + (uint32_t)dy * hw_pitch);
              for (uint32_t sx = 0; sx < cw; sx++) {
                int32_t dx = cx + (int32_t)sx;
                if (dx < 0 || dx >= (int32_t)width)
                  continue;
                uint32_t sp = src[sx];
                uint32_t a = sp >> 24;
                if (a == 0)
                  continue;
                if (a == 255) {
                  dst[dx] = sp;
                } else {
                  uint32_t dp = dst[dx];
                  uint32_t sr = (sp >> 16) & 0xff;
                  uint32_t sg = (sp >> 8) & 0xff;
                  uint32_t sb = sp & 0xff;
                  uint32_t dr = (dp >> 16) & 0xff;
                  uint32_t dg = (dp >> 8) & 0xff;
                  uint32_t db = dp & 0xff;
                  uint32_t r = (sr * a + dr * (255 - a)) / 255;
                  uint32_t g = (sg * a + dg * (255 - a)) / 255;
                  uint32_t b = (sb * a + db * (255 - a)) / 255;
                  dst[dx] = 0xff000000 | (r << 16) | (g << 8) | b;
                }
              }
            }
            __asm__ volatile("sfence" ::: "memory");
          }
        }
      }
    }
  }
  spinlock_release(&dev->lock);
}

/* ── Per-client open / close ─────────────────────────────────────────────── */

static int drm_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
  struct drm_file *file = node_to_file(node);
  struct drm_device *dev = file ? file->dev : NULL;
  if (!dev)
    return -9; /* EBADF */

  klog_puts("[DRM] ioctl request=0x");
  klog_hex32(request);
  klog_puts("\n");

  switch (request) {

  /* ── Version / caps ──────────────────────────────────────────────── */
  case DRM_IOCTL_VERSION: {
    struct drm_version *v = (struct drm_version *)arg;
    v->version_major = 1;
    v->version_minor = 0;
    v->version_patchlevel = 0;
    /* Always report the true string lengths so userspace can allocate */
    v->name_len = 12; /* "AscentOS DRM" */
    v->date_len = 8;  /* "20260528" */
    v->desc_len = 27; /* "AscentOS Graphics Subsystem" */
    /* Only write strings if userspace provided buffers */
    if (v->name && v->name_len > 0) {
      size_t copy = v->name_len < 13 ? v->name_len : 13;
      memcpy(v->name, "AscentOS DRM", copy);
      if (copy < v->name_len)
        v->name[copy] = '\0';
    }
    if (v->date && v->date_len > 0) {
      size_t copy = v->date_len < 9 ? v->date_len : 9;
      memcpy(v->date, "20260528", copy);
      if (copy < v->date_len)
        v->date[copy] = '\0';
    }
    if (v->desc && v->desc_len > 0) {
      size_t copy = v->desc_len < 28 ? v->desc_len : 28;
      memcpy(v->desc, "AscentOS Graphics Subsystem", copy);
      if (copy < v->desc_len)
        v->desc[copy] = '\0';
    }
    return 0;
  }
  case DRM_IOCTL_GET_CAP: {
    struct drm_get_cap *cap = (struct drm_get_cap *)arg;
    klog_puts("[DRM] GET_CAP capability=0x");
    klog_hex64(cap->capability);
    klog_puts("\n");
    switch (cap->capability) {
    case DRM_CAP_DUMB_BUFFER:
      cap->value = 1;
      break;
    case DRM_CAP_VBLANK_HIGH_CRTC:
      cap->value = 1;
      break;
    case DRM_CAP_DUMB_PREFER_SHADOW:
      cap->value = 1;
      break;
    case DRM_CAP_PRIME:
      cap->value = 3;
      break; /* (IMPORT | EXPORT) */
    case DRM_CAP_TIMESTAMP_MONOTONIC:
      cap->value = 1;
      break;
    case DRM_CAP_ASYNC_PAGE_FLIP:
      cap->value = 0;
      break;
    case 0x12:
      cap->value = 1;
      break; /* CRTC_IN_VBLANK_EVENT */
    case 0x13:
      cap->value = 0;
      break; /* SYNCOBJ not implemented */
    case 0x14:
      cap->value = 0;
      break; /* SYNCOBJ_TIMELINE not implemented */
    case 0x15:
      cap->value = 0;
      break; /* DRM_CAP_PAGE_FLIP_TARGET not implemented */
    case DRM_CAP_ADDFB2_MODIFIERS:
      cap->value = 0;
      break;
    case DRM_CAP_CURSOR_WIDTH:
      cap->value = 64;
      break;
    case DRM_CAP_CURSOR_HEIGHT:
      cap->value = 64;
      break;
    case DRM_CAP_ATOMIC:
      cap->value = 0;
      break;
    case 0x11:
      cap->value = 0;
      break; /* DRM_CAP_LESSOR (not supported) */
    default:
      cap->value = 0;
      break;
    }
    klog_puts("[DRM] GET_CAP value=0x");
    klog_hex64(cap->value);
    klog_puts("\n");
    return 0;
  }
  case 0x4010640D: /* DRM_IOCTL_SET_CLIENT_CAP */ {
    struct drm_set_client_cap *cap = (struct drm_set_client_cap *)arg;
    klog_puts("[DRM] SET_CLIENT_CAP cap=");
    klog_uint64(cap->capability);
    klog_puts(" val=");
    klog_uint64(cap->value);
    klog_puts("\n");
    uint32_t bit = 0;
    switch (cap->capability) {
    case DRM_CLIENT_CAP_STEREO_3D:
      bit = DRM_FILE_CAP_STEREO_3D;
      break;
    case DRM_CLIENT_CAP_UNIVERSAL_PLANES:
      bit = DRM_FILE_CAP_UNIVERSAL_PLANES;
      break;
    case DRM_CLIENT_CAP_ATOMIC:
      /* Linux DRM makes ATOMIC imply universal planes and aspect-ratio modes. */
      bit = DRM_FILE_CAP_ATOMIC | DRM_FILE_CAP_UNIVERSAL_PLANES |
            DRM_FILE_CAP_ASPECT_RATIO;
      break;
    case DRM_CLIENT_CAP_ASPECT_RATIO:
      bit = DRM_FILE_CAP_ASPECT_RATIO;
      break;
    case DRM_CLIENT_CAP_WRITEBACK_CONNECTORS:
      /* No writeback connectors exist, but enabling visibility is harmless. */
      bit = DRM_FILE_CAP_WRITEBACK_CONNECTORS;
      break;
    default:
      klog_puts("[DRM] SET_CLIENT_CAP unsupported\n");
      return -95; /* EOPNOTSUPP */
    }

    if (cap->value)
      file->client_caps |= bit;
    else
      file->client_caps &= ~bit;
    return 0;
  }
  case DRM_IOCTL_SET_MASTER:
    file->is_master = 1;
    return 0;
  case DRM_IOCTL_DROP_MASTER:
    file->is_master = 0;
    return 0;
  case 0x40046411: /* DRM_IOCTL_AUTH_MAGIC */
    return 0;
  case 0x80046402: /* DRM_IOCTL_GET_MAGIC */ {
    uint32_t *magic = (uint32_t *)arg;
    *magic = 0x1234; /* dummy magic */
    return 0;
  }
  case 0x80086406: /* DRM_IOCTL_GET_STATS */
    klog_puts("[DRM] GET_STATS -> ENOTTY\n");
    return -25; /* ENOTTY */
  case DRM_IOCTL_MODE_CREATE_LEASE:
    klog_puts("[DRM] MODE_CREATE_LEASE -> EINVAL\n");
    return -22; /* EINVAL: Leasing not supported on this driver version */

  /* ── Per-client GEM ──────────────────────────────────────────────── */
  case DRM_IOCTL_GEM_CREATE: {
    struct drm_gem_create *c = (struct drm_gem_create *)arg;
    struct drm_gem_object *obj = drm_gem_object_create(dev, c->size);
    if (!obj)
      return -12; /* ENOMEM */
    /* Register in global list AND per-client table */
    uint32_t local_h = drm_file_gem_register(file, obj);
    if (!local_h) {
      drm_gem_object_free(dev, obj);
      return -12;
    }
    c->handle = local_h;
    klog_puts("[DRM] GEM_CREATE size=");
    klog_uint64(c->size);
    klog_puts(" handle=");
    klog_uint64(c->handle);
    klog_puts(" phys=0x");
    klog_hex64(obj->phys_addr);
    klog_puts("\n");
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
    klog_puts("[DRM] CREATE_DUMB in width=");
    klog_uint64(c->width);
    klog_puts(" height=");
    klog_uint64(c->height);
    klog_puts(" bpp=");
    klog_uint64(c->bpp);
    klog_puts(" flags=0x");
    klog_hex32(c->flags);
    klog_puts("\n");
    c->pitch = (c->width * (c->bpp / 8) + 63) & ~63;
    c->size = (uint64_t)c->pitch * c->height;
    struct drm_gem_object *obj = drm_gem_object_create(dev, c->size);
    if (!obj)
      return -12; /* ENOMEM */
    uint32_t local_h = drm_file_gem_register(file, obj);
    if (!local_h) {
      drm_gem_object_free(dev, obj);
      return -12;
    }
    c->handle = local_h;
    klog_puts("[DRM] CREATE_DUMB out handle=");
    klog_uint64(c->handle);
    klog_puts(" pitch=");
    klog_uint64(c->pitch);
    klog_puts(" size=");
    klog_uint64(c->size);
    klog_puts(" gem_phys=0x");
    klog_hex64(obj->phys_addr);
    klog_puts(" gem_virt=0x");
    klog_hex64((uint64_t)obj->virt_addr);
    klog_puts("\n");
    return 0;
  }
  case DRM_IOCTL_MODE_MAP_DUMB: {
    struct drm_mode_map_dumb *m = (struct drm_mode_map_dumb *)arg;
    klog_puts("[DRM] MAP_DUMB handle=");
    klog_uint64(m->handle);
    klog_puts("\n");
    struct drm_gem_object *obj = drm_file_gem_lookup(file, m->handle);
    if (!obj)
      return -2; /* ENOENT */
    m->offset = obj->phys_addr | 0x1000000000000000ULL;
    klog_puts("[DRM] MAP_DUMB out offset=0x");
    klog_hex64(m->offset);
    klog_puts(" size=");
    klog_uint64(obj->size);
    klog_puts("\n");
    return 0;
  }
  case DRM_IOCTL_MODE_DESTROY_DUMB: {
    uint32_t handle = *(uint32_t *)arg;
    drm_file_gem_release(file, handle);
    return 0;
  }

  /* ── KMS resource queries ────────────────────────────────────────── */
  case DRM_IOCTL_MODE_GETRESOURCES: {
    struct drm_mode_card_res *res = (struct drm_mode_card_res *)arg;
    uint32_t fbs = 0, crtcs = 0, connectors = 0, encoders = 0;
    struct drm_mode_object *mobj;
    spinlock_acquire(&dev->lock);
    list_for_each_entry(mobj, &dev->kms_objects, list) {
      if (mobj->type == DRM_MODE_OBJECT_CRTC) {
        if (res->crtc_id_ptr && crtcs < res->count_crtcs)
          ((uint32_t *)res->crtc_id_ptr)[crtcs] = mobj->id;
        crtcs++;
      } else if (mobj->type == DRM_MODE_OBJECT_CONNECTOR) {
        if (res->connector_id_ptr && connectors < res->count_connectors) {
          ((uint32_t *)res->connector_id_ptr)[connectors] = mobj->id;
          klog_puts("[DRM] GETRESOURCES: filling connector id=");
          klog_uint64(mobj->id);
          klog_puts(" at ptr=");
          klog_hex64(res->connector_id_ptr + connectors * 4);
          klog_puts("\n");
        }
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
    klog_puts("[DRM] GETRESOURCES: fbs=");
    klog_uint64(fbs);
    klog_puts(" crtcs=");
    klog_uint64(crtcs);
    klog_puts(" connectors=");
    klog_uint64(connectors);
    klog_puts(" encoders=");
    klog_uint64(encoders);
    klog_puts("\n");

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

  klog_puts("[DRM] GETPLANERESOURCES in count=");
  klog_uint64(res->count_planes);
  klog_puts(" ptr=0x");
  klog_hex64(res->plane_id_ptr);
  klog_puts("\n");

  spinlock_acquire(&dev->lock);

  list_for_each_entry(mobj, &dev->kms_objects, list) {
    if (mobj->type == DRM_MODE_OBJECT_PLANE) {
      uint64_t type_val = 999;
      drm_obj_get_prop(mobj, DRM_PROP_ID_TYPE, &type_val);

      klog_puts("[DRM] GETPLANERESOURCES found plane index=");
      klog_uint64(planes);
      klog_puts(" id=");
      klog_uint64(mobj->id);
      klog_puts(" type=");
      klog_uint64(type_val);
      klog_puts("\n");

      if (res->plane_id_ptr && planes < res->count_planes) {
        ((uint32_t *)res->plane_id_ptr)[planes] = mobj->id;

        klog_puts("[DRM] GETPLANERESOURCES wrote index=");
        klog_uint64(planes);
        klog_puts(" id=");
        klog_uint64(mobj->id);
        klog_puts("\n");
      }

      planes++;
    }
  }

  spinlock_release(&dev->lock);

  res->count_planes = planes;

  klog_puts("[DRM] GETPLANERESOURCES out count=");
  klog_uint64(planes);
  klog_puts("\n");

  return 0;
}
  case 0xC02064B6: { /* DRM_IOCTL_MODE_GETPLANE */
  struct {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_formats;
    uint64_t format_type_ptr;
  } *p = (void *)arg;

  klog_puts("[DRM] GETPLANE in id=");
  klog_uint64(p->plane_id);
  klog_puts(" format_ptr=0x");
  klog_hex64(p->format_type_ptr);
  klog_puts(" count_in=");
  klog_uint64(p->count_formats);
  klog_puts("\n");

  spinlock_acquire(&dev->lock);

  struct drm_mode_object *mobj = drm_mode_object_find(dev, p->plane_id);
  if (!mobj || mobj->type != DRM_MODE_OBJECT_PLANE) {
    spinlock_release(&dev->lock);
    klog_puts("[DRM] GETPLANE failed: bad plane id\n");
    return -2; /* ENOENT */
  }

  struct drm_plane *plane = (struct drm_plane *)mobj;

  uint64_t type_val = 0;
  if (drm_obj_get_prop(&plane->base, DRM_PROP_ID_TYPE, &type_val) != 0)
    type_val = DRM_PLANE_TYPE_OVERLAY;

  uint64_t crtc_id = 0;
  uint64_t fb_id = 0;
  drm_obj_get_prop(&plane->base, DRM_PROP_ID_CRTC_ID, &crtc_id);
  drm_obj_get_prop(&plane->base, DRM_PROP_ID_FB_ID, &fb_id);

  p->crtc_id = (uint32_t)crtc_id;
  p->fb_id = (uint32_t)fb_id;
  p->possible_crtcs = plane->possible_crtcs;
  p->gamma_size = 0;

  uint32_t formats[2];

  if ((uint32_t)type_val == DRM_PLANE_TYPE_CURSOR) {
    formats[0] = 0x34325241; /* ARGB8888 */
    formats[1] = 0x34325258; /* XRGB8888 */
    p->count_formats = 2;
  } else {
    formats[0] = 0x34325258; /* XRGB8888 */
    formats[1] = 0x34325241; /* ARGB8888 */
    p->count_formats = 2;
  }

  if (p->format_type_ptr) {
    ((uint32_t *)p->format_type_ptr)[0] = formats[0];
    ((uint32_t *)p->format_type_ptr)[1] = formats[1];
  }

  klog_puts("[DRM] GETPLANE out id=");
  klog_uint64(p->plane_id);
  klog_puts(" type=");
  klog_uint64(type_val);
  klog_puts(" possible_crtcs=0x");
  klog_hex32(p->possible_crtcs);
  klog_puts(" formats=");
  klog_uint64(p->count_formats);
  klog_puts("\n");

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
    c->mode.hdisplay = fb_get_width();
    c->mode.hsync_start = c->mode.hdisplay + 8;
    c->mode.hsync_end = c->mode.hdisplay + 16;
    c->mode.htotal = c->mode.hdisplay + 32;
    c->mode.vdisplay = fb_get_height();
    c->mode.vsync_start = c->mode.vdisplay + 4;
    c->mode.vsync_end = c->mode.vdisplay + 8;
    c->mode.vtotal = c->mode.vdisplay + 12;
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
    klog_puts("[DRM] GETCONNECTOR: arg=");
    klog_hex64((uint64_t)arg);
    klog_puts(" id_in_struct=");
    klog_uint64(c->connector_id);
    klog_puts("\n");
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *mobj = drm_mode_object_find(dev, c->connector_id);
    if (!mobj || mobj->type != DRM_MODE_OBJECT_CONNECTOR) {
      klog_puts("[DRM] GETCONNECTOR: object not found or wrong type: id=");
      klog_uint64(c->connector_id);
      klog_puts("\n");
      spinlock_release(&dev->lock);
      return -2; /* ENOENT */
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

    /* Expose connector properties — wlroots needs CRTC_ID on the connector */
    uint32_t prop_count = mobj->prop_count;
    if (c->props_ptr && c->count_props >= prop_count) {
      uint32_t *prop_ids = (uint32_t *)c->props_ptr;
      uint64_t *prop_vals = (uint64_t *)c->prop_values_ptr;
      for (uint32_t i = 0; i < prop_count; i++) {
        prop_ids[i] = mobj->props[i].prop_id;
        prop_vals[i] = mobj->props[i].value;
      }
    }
    c->count_props = prop_count;

    c->count_modes = 1;
    if (c->modes_ptr) {
      struct drm_mode_modeinfo *m = (struct drm_mode_modeinfo *)c->modes_ptr;
      m->clock = 60000;
      m->hdisplay = fb_get_width();
      m->hsync_start = m->hdisplay + 8;
      m->hsync_end = m->hdisplay + 16;
      m->htotal = m->hdisplay + 32;
      m->vdisplay = fb_get_height();
      m->vsync_start = m->vdisplay + 4;
      m->vsync_end = m->vdisplay + 8;
      m->vtotal = m->vdisplay + 12;
      m->vrefresh = 60;
      m->flags = 0;
      m->type = 0x48; /* DRM_MODE_TYPE_DRIVER | PREFERRED */
      strcpy(m->name, "Native");
    }
    c->count_encoders = 0;
    if (conn->encoder) {
      c->count_encoders = 1;
      if (c->encoders_ptr)
        ((uint32_t *)c->encoders_ptr)[0] = conn->encoder->base.id;
    }
    klog_puts("[DRM] GETCONNECTOR out id=");
    klog_uint64(c->connector_id);
    klog_puts(" conn=");
    klog_uint64(c->connection);
    klog_puts(" enc=");
    klog_uint64(c->encoder_id);
    klog_puts(" modes=");
    klog_uint64(c->count_modes);
    klog_puts(" props=");
    klog_uint64(c->count_props);
    klog_puts(" encoders=");
    klog_uint64(c->count_encoders);
    klog_puts(" modes_ptr=0x");
    klog_hex64(c->modes_ptr);
    klog_puts(" props_ptr=0x");
    klog_hex64(c->props_ptr);
    klog_puts("\n");
    spinlock_release(&dev->lock);
    return 0;
  }

  /* ── Framebuffer management ──────────────────────────────────────── */
  case DRM_IOCTL_MODE_ADDFB: {
    struct drm_mode_fb_cmd *cmd = (struct drm_mode_fb_cmd *)arg;
    klog_puts("[DRM] ADDFB in handle=");
    klog_uint64(cmd->handle);
    klog_puts(" width=");
    klog_uint64(cmd->width);
    klog_puts(" height=");
    klog_uint64(cmd->height);
    klog_puts(" pitch=");
    klog_uint64(cmd->pitch);
    klog_puts(" bpp=");
    klog_uint64(cmd->bpp);
    klog_puts(" depth=");
    klog_uint64(cmd->depth);
    klog_puts("\n");
    /* Resolve local handle → global gem object */
    struct drm_gem_object *gem = drm_file_gem_lookup(file, cmd->handle);
    if (!gem) {
      klog_puts("[DRM] ADDFB missing GEM handle=");
      klog_uint64(cmd->handle);
      klog_puts("\n");
      return -2; /* ENOENT */
    }
    /* Temporarily patch handle to global for drm_framebuffer_create */
    uint32_t saved = cmd->handle;
    cmd->handle = gem->handle;
    struct drm_framebuffer *fb = drm_framebuffer_create(dev, cmd);
    cmd->handle = saved;
    if (!fb) {
      klog_puts("[DRM] ADDFB framebuffer create failed\n");
      return -1;
    }
    cmd->fb_id = fb->base.id;
    klog_puts("[DRM] ADDFB out fb_id=");
    klog_uint64(cmd->fb_id);
    klog_puts(" gem_phys=0x");
    klog_hex64(gem->phys_addr);
    klog_puts("\n");
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
  case DRM_IOCTL_MODE_ADDFB2:
    /* Full multi-planar path — resolves handles via global gem list */
    return drm_ioctl_addfb2(file, dev, arg);

  /* ── Legacy modesetting ──────────────────────────────────────────── */
  case DRM_IOCTL_MODE_SETCRTC: {
    struct drm_mode_crtc *crtc_cmd = (struct drm_mode_crtc *)arg;
    klog_puts("[DRM] SETCRTC crtc=");
    klog_uint64(crtc_cmd->crtc_id);
    klog_puts(" fb=");
    klog_uint64(crtc_cmd->fb_id);
    klog_puts(" connectors=");
    klog_uint64(crtc_cmd->count_connectors);
    klog_puts(" mode_valid=");
    klog_uint64(crtc_cmd->mode_valid);
    klog_puts(" mode=");
    klog_uint64(crtc_cmd->mode.hdisplay);
    klog_puts("x");
    klog_uint64(crtc_cmd->mode.vdisplay);
    klog_puts("\n");
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *crtc_obj =
        drm_mode_object_find(dev, crtc_cmd->crtc_id);
    struct drm_mode_object *fb_obj = drm_mode_object_find(dev, crtc_cmd->fb_id);
    if (!crtc_obj || crtc_obj->type != DRM_MODE_OBJECT_CRTC) {
      klog_puts("[DRM] SETCRTC bad crtc id\n");
      spinlock_release(&dev->lock);
      return -1;
    }
    if (crtc_cmd->fb_id && (!fb_obj || fb_obj->type != DRM_MODE_OBJECT_FB))
      klog_puts("[DRM] SETCRTC warning: fb id not found\n");
    struct drm_crtc *crtc = (struct drm_crtc *)crtc_obj;
    if (fb_obj && fb_obj->type == DRM_MODE_OBJECT_FB)
      crtc->fb = (struct drm_framebuffer *)fb_obj;
    spinlock_release(&dev->lock);
    drm_commit(dev);
    return 0;
  }
  case DRM_IOCTL_MODE_PAGE_FLIP: {
    struct drm_mode_crtc_page_flip *flip =
        (struct drm_mode_crtc_page_flip *)arg;
    klog_puts("[DRM] PAGE_FLIP crtc=");
    klog_uint64(flip->crtc_id);
    klog_puts(" fb=");
    klog_uint64(flip->fb_id);
    klog_puts(" flags=0x");
    klog_hex32(flip->flags);
    klog_puts(" user_data=0x");
    klog_hex64(flip->user_data);
    klog_puts("\n");
    spinlock_acquire(&dev->lock);
    struct drm_mode_object *crtc_obj = drm_mode_object_find(dev, flip->crtc_id);
    struct drm_mode_object *fb_obj = drm_mode_object_find(dev, flip->fb_id);
    if (!crtc_obj || crtc_obj->type != DRM_MODE_OBJECT_CRTC || !fb_obj ||
        fb_obj->type != DRM_MODE_OBJECT_FB) {
      klog_puts("[DRM] PAGE_FLIP rejected: bad crtc or fb\n");
      spinlock_release(&dev->lock);
      return -1;
    }
    struct drm_crtc *crtc = (struct drm_crtc *)crtc_obj;
    crtc->fb = (struct drm_framebuffer *)fb_obj;
    if (flip->flags & DRM_MODE_PAGE_FLIP_EVENT) {
      struct drm_event_vblank ev = {0};
      ev.base.type = DRM_EVENT_FLIP_COMPLETE;
      ev.base.length = sizeof(ev);
      ev.user_data = flip->user_data;
      drm_fill_vblank_event(&ev, flip->crtc_id);
      spinlock_release(&dev->lock);
      drm_commit(dev);
      drm_file_send_event(file, &ev, node);
      return 0;
    }
    spinlock_release(&dev->lock);
    drm_commit(dev);
    return 0;
  }

  /* ── Atomic modesetting ──────────────────────────────────────────── */
  case DRM_IOCTL_MODE_ATOMIC: {
    int ret = drm_ioctl_atomic(node, file, dev, arg);
    if (ret == 0 &&
        !(((struct drm_mode_atomic *)arg)->flags & DRM_MODE_ATOMIC_TEST_ONLY)) {
      klog_puts("[DRM] ATOMIC commit triggering drm_commit\n");
      drm_commit(dev);
    }
    return ret;
  }
  case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
    return drm_ioctl_obj_getprops(dev, arg);
  case DRM_IOCTL_MODE_GETPROPERTY:
    return drm_ioctl_getproperty(dev, arg);
  case DRM_IOCTL_MODE_SETPROPERTY:
    return 0; /* stub */
  case DRM_IOCTL_MODE_DIRTYFB:
    drm_commit(dev);
    return 0;
  case DRM_IOCTL_MODE_CREATEPROPBLOB: {
    struct drm_mode_create_blob *b = (struct drm_mode_create_blob *)arg;
    if (!b->data || !b->length)
      return -14; /* EFAULT */
    struct drm_prop_blob *blob =
        drm_blob_create(dev, (void *)b->data, b->length);
    if (!blob)
      return -12; /* ENOMEM */
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
    if (!obj)
      return -2; /* ENOENT */
    int prime_fd = drm_prime_export(obj);
    if (prime_fd < 0)
      return -1; /* generic error for now */
    p->fd = prime_fd;
    return 0;
  }
  case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
    struct drm_prime_handle *p = (struct drm_prime_handle *)arg;
    struct drm_gem_object *obj = drm_prime_import(p->fd);
    if (!obj)
      return -2; /* ENOENT */
    uint32_t local_h = drm_file_gem_register(file, obj);
    if (!local_h)
      return -12; /* ENOMEM */
    p->handle = local_h;
    return 0;
  }

  case DRM_IOCTL_WAIT_VBLANK: {
    union drm_wait_vblank *vbl = (union drm_wait_vblank *)arg;
    uint32_t type = vbl->request.type;
    uint64_t user_data = vbl->request.signal;
    uint64_t ms = lapic_timer_get_ms();
    uint32_t seq = drm_event_sequence++;
    klog_puts("[DRM] WAIT_VBLANK type=0x");
    klog_hex32(type);
    klog_puts(" seq_in=");
    klog_uint64(vbl->request.sequence);
    klog_puts("\n");
    vbl->reply.type = type;
    vbl->reply.sequence = seq;
    vbl->reply.tval_sec = (long)(ms / 1000);
    vbl->reply.tval_usec = (long)((ms % 1000) * 1000);
    if (type & DRM_VBLANK_EVENT) {
      struct drm_event_vblank ev = {0};
      ev.base.type = DRM_EVENT_VBLANK;
      ev.base.length = sizeof(ev);
      ev.user_data = user_data;
      ev.tv_sec = (uint32_t)vbl->reply.tval_sec;
      ev.tv_usec = (uint32_t)vbl->reply.tval_usec;
      ev.sequence = seq;
      ev.crtc_id = 0;
      drm_file_send_event(file, &ev, node);
    }
    return 0;
  }

case DRM_IOCTL_MODE_CURSOR: {
  struct drm_mode_cursor *cur = (struct drm_mode_cursor *)arg;

  klog_puts("[DRM] MODE_CURSOR flags=0x");
  klog_hex32(cur->flags);
  klog_puts(" handle=");
  klog_uint64(cur->handle);
  klog_puts(" x=");
  klog_uint64((uint32_t)cur->x);
  klog_puts(" y=");
  klog_uint64((uint32_t)cur->y);
  klog_puts(" w=");
  klog_uint64(cur->width);
  klog_puts(" h=");
  klog_uint64(cur->height);
  klog_puts("\n");

  spinlock_acquire(&dev->lock);

  struct drm_mode_object *obj;
  list_for_each_entry(obj, &dev->kms_objects, list) {
    if (obj->type != DRM_MODE_OBJECT_CRTC)
      continue;

    struct drm_crtc *crtc = (struct drm_crtc *)obj;
    struct drm_plane *cursor = crtc->cursor;

    if (!cursor)
      continue;

    uint32_t flags = cur->flags ? cur->flags :
        (DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE);

    if (flags & DRM_MODE_CURSOR_MOVE) {
      cursor->crtc_x = cur->x;
      cursor->crtc_y = cur->y;
    }

    if (flags & DRM_MODE_CURSOR_BO) {
      if (cur->width)
        cursor->crtc_w = cur->width;
      if (cur->height)
        cursor->crtc_h = cur->height;

      if (cur->handle == 0) {
        cursor->fb = NULL;
        break;
      }

      struct drm_gem_object *gem = drm_file_gem_lookup(file, cur->handle);
      if (!gem) {
        spinlock_release(&dev->lock);
        klog_puts("[DRM] MODE_CURSOR invalid GEM handle\n");
        return -2; /* ENOENT */
      }

      static struct drm_framebuffer legacy_cursor_fb;
      memset(&legacy_cursor_fb, 0, sizeof(legacy_cursor_fb));

      legacy_cursor_fb.width = cur->width ? cur->width : 64;
      legacy_cursor_fb.height = cur->height ? cur->height : 64;
      legacy_cursor_fb.pitch = legacy_cursor_fb.width * 4;
      legacy_cursor_fb.bpp = 32;
      legacy_cursor_fb.gem_obj = gem;

      cursor->fb = &legacy_cursor_fb;
    }
    break;
  }

  spinlock_release(&dev->lock);
  drm_commit(dev);
  return 0;
}

case DRM_IOCTL_MODE_CURSOR2: {
  struct drm_mode_cursor2 *cur = (struct drm_mode_cursor2 *)arg;

  klog_puts("[DRM] MODE_CURSOR2 flags=0x");
  klog_hex32(cur->flags);
  klog_puts(" handle=");
  klog_uint64(cur->handle);
  klog_puts(" x=");
  klog_uint64((uint32_t)cur->x);
  klog_puts(" y=");
  klog_uint64((uint32_t)cur->y);
  klog_puts(" w=");
  klog_uint64(cur->width);
  klog_puts(" h=");
  klog_uint64(cur->height);
  klog_puts(" hot=");
  klog_uint64((uint32_t)cur->hot_x);
  klog_puts(",");
  klog_uint64((uint32_t)cur->hot_y);
  klog_puts("\n");

  spinlock_acquire(&dev->lock);

  struct drm_mode_object *obj;
  list_for_each_entry(obj, &dev->kms_objects, list) {
    if (obj->type != DRM_MODE_OBJECT_CRTC)
      continue;

    struct drm_crtc *crtc = (struct drm_crtc *)obj;
    struct drm_plane *cursor = crtc->cursor;

    if (!cursor)
      continue;

    uint32_t flags = cur->flags ? cur->flags :
        (DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE);

    if (flags & DRM_MODE_CURSOR_MOVE) {
      cursor->crtc_x = cur->x;
      cursor->crtc_y = cur->y;
    }

    if (flags & DRM_MODE_CURSOR_BO) {
      if (cur->width)
        cursor->crtc_w = cur->width;
      if (cur->height)
        cursor->crtc_h = cur->height;

      if (cur->handle == 0) {
        cursor->fb = NULL;
        break;
      }

      struct drm_gem_object *gem = drm_file_gem_lookup(file, cur->handle);
      if (!gem) {
        spinlock_release(&dev->lock);
        klog_puts("[DRM] MODE_CURSOR2 invalid GEM handle\n");
        return -2; /* ENOENT */
      }

      static struct drm_framebuffer legacy_cursor2_fb;
      memset(&legacy_cursor2_fb, 0, sizeof(legacy_cursor2_fb));

      legacy_cursor2_fb.width = cur->width ? cur->width : 64;
      legacy_cursor2_fb.height = cur->height ? cur->height : 64;
      legacy_cursor2_fb.pitch = legacy_cursor2_fb.width * 4;
      legacy_cursor2_fb.bpp = 32;
      legacy_cursor2_fb.gem_obj = gem;

      cursor->fb = &legacy_cursor2_fb;
    }
    break;
  }

  spinlock_release(&dev->lock);
  drm_commit(dev);
  return 0;
}

  case DRM_IOCTL_MODE_GETGAMMA:
  case DRM_IOCTL_MODE_SETGAMMA:
    return 0; /* stub */
  default:
    return -25; /* ENOTTY */
  }
}

/* ── mmap — resolve GEM offset to physical pages ─────────────────────────── */

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

  uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW |
                        PAGE_FLAG_PWT | PAGE_FLAG_PAT;
  uint32_t num_pages = (length + 4095) / 4096;
  uint64_t *pml4 = vmm_get_active_pml4();
  for (uint32_t i = 0; i < num_pages; i++)
    vmm_map_page(pml4, vaddr + i * 4096, phys + i * 4096, page_flags);
  return vaddr;
}

/* ── poll / read — per-client event queue ────────────────────────────────── */

static int drm_poll(struct vfs_node *node, int events) {
  struct drm_file *file = node_to_file(node);
  int revents = 0x0004; /* POLLOUT always */
  if (file) {
    spinlock_acquire(&file->lock);
    if (!list_empty(&file->event_queue))
      revents |= 0x0001; /* POLLIN */
    spinlock_release(&file->lock);
  }
  return revents & events;
}

static uint32_t drm_read(struct vfs_node *node, uint32_t offset,
                         uint32_t length, uint8_t *buffer) {
  struct drm_file *file = node_to_file(node);
  (void)offset;
  if (!file)
    return 0;

  spinlock_acquire(&file->lock);
  while (list_empty(&file->event_queue)) {
    spinlock_release(&file->lock);
    struct thread *current = sched_get_current();
    wait_queue_entry_t entry = {.thread = current, .next = NULL};
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
  if (length < event_size) {
    spinlock_release(&file->lock);
    return 0;
  }
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
  if (strcmp(name, "card0") != 0)
    return NULL;

  /* Allocate per-client drm_file */
  struct drm_file *file = drm_file_alloc(&global_drm_dev);
  if (!file)
    return NULL;

  /* Allocate a fresh non-persistent clone node */
  vfs_node_t *clone = kmalloc(sizeof(vfs_node_t));
  if (!clone) {
    drm_file_free(file);
    return NULL;
  }
  vfs_node_init(clone);
  strcpy(clone->name, "card0");
  clone->flags = FS_CHARDEV; /* non-persistent: freed when refcount→0 */
  clone->mask = 0666;
  clone->inode = (226U << 8) | 0U; /* makedev(226,0) — DRM major:minor */
  clone->device = file;            /* ← per-client state */
  clone->ioctl = drm_ioctl;
  clone->mmap = drm_vfs_mmap;
  clone->open = drm_open;
  clone->close = drm_close;
  clone->read = drm_read;
  clone->poll = drm_poll;
  clone->wait_queue = &file->event_wq;
  clone->refcount = 0; /* vfs_open will bump to 1 */

  return clone;
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void drm_init(void) {
  memset(&global_drm_dev, 0, sizeof(struct drm_device));
  global_drm_dev.name = "card0";
  global_drm_dev.next_gem_handle = 1;
  global_drm_dev.next_kms_id = 1000;
  global_drm_dev.next_blob_id = 1;
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
      klog_puts("[DRM] Bridged HW framebuffer to GEM handle 0xF0B0\n");
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

  /* Override finddir on the dri directory to return per-client clones */
  dri_dir->finddir = drm_dri_finddir;

  klog_puts("[DRM] Registered /dev/dri/card0 (per-client mode)\n");
}
