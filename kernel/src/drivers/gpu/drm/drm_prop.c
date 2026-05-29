#include "../../../console/klog.h"
#include "../../../fb/framebuffer.h"
#include "../../../lib/string.h"
#include "../../../mm/heap.h"
#include "drm.h"

/* ── Global property catalogue ──────────────────────────────────────────── */

static const struct drm_property_def drm_prop_catalogue[] = {
    /* Plane properties */
    {DRM_PROP_ID_CRTC_ID, DRM_PROP_FLAG_ATOMIC, "CRTC_ID", 0, UINT32_MAX},
    {DRM_PROP_ID_FB_ID, DRM_PROP_FLAG_ATOMIC, "FB_ID", 0, UINT32_MAX},
    {DRM_PROP_ID_SRC_X, DRM_PROP_FLAG_ATOMIC, "SRC_X", 0, UINT32_MAX},
    {DRM_PROP_ID_SRC_Y, DRM_PROP_FLAG_ATOMIC, "SRC_Y", 0, UINT32_MAX},
    {DRM_PROP_ID_SRC_W, DRM_PROP_FLAG_ATOMIC, "SRC_W", 0, UINT32_MAX},
    {DRM_PROP_ID_SRC_H, DRM_PROP_FLAG_ATOMIC, "SRC_H", 0, UINT32_MAX},
    {DRM_PROP_ID_CRTC_X, DRM_PROP_FLAG_ATOMIC, "CRTC_X", 0, UINT32_MAX},
    {DRM_PROP_ID_CRTC_Y, DRM_PROP_FLAG_ATOMIC, "CRTC_Y", 0, UINT32_MAX},
    {DRM_PROP_ID_CRTC_W, DRM_PROP_FLAG_ATOMIC, "CRTC_W", 0, UINT32_MAX},
    {DRM_PROP_ID_CRTC_H, DRM_PROP_FLAG_ATOMIC, "CRTC_H", 0, UINT32_MAX},
    /* CRTC properties */
    {DRM_PROP_ID_ACTIVE, DRM_PROP_FLAG_ATOMIC, "ACTIVE", 0, 1},
    {DRM_PROP_ID_MODE_ID, DRM_PROP_FLAG_ATOMIC | DRM_PROP_TYPE_BLOB, "MODE_ID",
     0, UINT32_MAX},
    /* Connector properties */
    {DRM_PROP_ID_DPMS, 0, "DPMS", 0, 3},
    {DRM_PROP_ID_CONNECTOR_ID, DRM_PROP_FLAG_ATOMIC | DRM_PROP_FLAG_IMMUTABLE,
     "CONNECTOR_ID", 0, UINT32_MAX},
    /* Plane type property */
    {DRM_PROP_ID_TYPE, DRM_PROP_TYPE_ENUM | DRM_PROP_FLAG_IMMUTABLE, "type", 0,
     2},
    /* Note: DRM_PROP_ID_CRTC_ID is shared between planes and connectors —
     * the same catalogue entry covers both (name "CRTC_ID", atomic flag). */
};

#define PROP_CATALOGUE_SIZE                                                    \
  (sizeof(drm_prop_catalogue) / sizeof(drm_prop_catalogue[0]))

const struct drm_property_def *drm_prop_find_def(uint32_t prop_id) {
  for (size_t i = 0; i < PROP_CATALOGUE_SIZE; i++) {
    if (drm_prop_catalogue[i].id == prop_id)
      return &drm_prop_catalogue[i];
  }
  return NULL;
}

/* ── Object property helpers ─────────────────────────────────────────────── */

/* Attach a property with its default value to a mode object */
void drm_obj_add_prop(struct drm_mode_object *obj, uint32_t prop_id,
                      uint64_t default_val) {
  if (obj->prop_count >= DRM_MAX_OBJ_PROPS)
    return;
  obj->props[obj->prop_count].prop_id = prop_id;
  obj->props[obj->prop_count].value = default_val;
  obj->prop_count++;
}

/* Get a property value from an object; returns -1 if not found */
int drm_obj_get_prop(struct drm_mode_object *obj, uint32_t prop_id,
                     uint64_t *out) {
  for (uint32_t i = 0; i < obj->prop_count; i++) {
    if (obj->props[i].prop_id == prop_id) {
      *out = obj->props[i].value;
      return 0;
    }
  }
  return -1;
}

/* Set a property value on an object; returns -1 if not found */
int drm_obj_set_prop(struct drm_mode_object *obj, uint32_t prop_id,
                     uint64_t value) {
  for (uint32_t i = 0; i < obj->prop_count; i++) {
    if (obj->props[i].prop_id == prop_id) {
      obj->props[i].value = value;
      return 0;
    }
  }
  return -1;
}

/* ── Blob management ─────────────────────────────────────────────────────── */

struct drm_prop_blob *drm_blob_create(struct drm_device *dev, const void *data,
                                      uint32_t length) {
  struct drm_prop_blob *blob = kmalloc(sizeof(struct drm_prop_blob));
  if (!blob)
    return NULL;

  blob->data = kmalloc(length);
  if (!blob->data) {
    kfree(blob);
    return NULL;
  }

  memcpy(blob->data, data, length);
  blob->length = length;

  spinlock_acquire(&dev->lock);
  blob->id = dev->next_blob_id++;
  list_add_tail(&blob->list, &dev->blob_objects);
  spinlock_release(&dev->lock);

  return blob;
}

struct drm_prop_blob *drm_blob_find(struct drm_device *dev, uint32_t id) {
  struct drm_prop_blob *b;
  list_for_each_entry(b, &dev->blob_objects, list) {
    if (b->id == id)
      return b;
  }
  return NULL;
}

void drm_blob_destroy(struct drm_device *dev, uint32_t id) {
  spinlock_acquire(&dev->lock);
  struct drm_prop_blob *b;
  list_for_each_entry(b, &dev->blob_objects, list) {
    if (b->id == id) {
      list_del(&b->list);
      spinlock_release(&dev->lock);
      kfree(b->data);
      kfree(b);
      return;
    }
  }
  spinlock_release(&dev->lock);
}

/* ── DRM_IOCTL_MODE_OBJ_GETPROPERTIES ───────────────────────────────────── */

int drm_ioctl_obj_getprops(struct drm_device *dev, uint64_t arg) {
  struct drm_mode_obj_get_properties *req =
      (struct drm_mode_obj_get_properties *)arg;

  spinlock_acquire(&dev->lock);
  struct drm_mode_object *mobj = NULL;
  struct drm_mode_object *iter;
  list_for_each_entry(iter, &dev->kms_objects, list) {
    if (iter->id == req->obj_id) {
      mobj = iter;
      break;
    }
  }
  if (!mobj) {
    spinlock_release(&dev->lock);
    return -2;
  } /* ENOENT */

  uint32_t count = mobj->prop_count;
  if (req->props_ptr && req->count_props >= count) {
    uint32_t *prop_ids = (uint32_t *)req->props_ptr;
    uint64_t *prop_vals = (uint64_t *)req->prop_values_ptr;
    for (uint32_t i = 0; i < count; i++) {
      prop_ids[i] = mobj->props[i].prop_id;
      prop_vals[i] = mobj->props[i].value;
    }
  }
  req->count_props = count;
  spinlock_release(&dev->lock);
  return 0;
}

/* ── DRM_IOCTL_MODE_GETPROPERTY ─────────────────────────────────────────── */

int drm_ioctl_getproperty(struct drm_device *dev, uint64_t arg) {
  (void)dev;
  struct {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[32];
    uint32_t count_values;
    uint32_t count_enum_blobs;
  } *p = (void *)arg;

  const struct drm_property_def *def = drm_prop_find_def(p->prop_id);
  if (!def) {
    /* Unknown property — return a harmless stub so userland doesn't crash */
    p->flags = 0;
    p->count_values = 0;
    p->count_enum_blobs = 0;
    strncpy(p->name, "Unknown", 32);
    return 0;
  }

  p->flags = def->flags;
  p->count_enum_blobs = 0;
  strncpy(p->name, def->name, 32);

  /* For RANGE properties expose [min, max] */
  if ((def->flags & 0x3f) == DRM_PROP_TYPE_RANGE) {
    p->count_values = 2;
    if (p->values_ptr) {
      uint64_t *vals = (uint64_t *)p->values_ptr;
      vals[0] = def->min_val;
      vals[1] = def->max_val;
    }
  } else if ((def->flags & 0x3f) == DRM_PROP_TYPE_ENUM) {
    /* Special case for "type" enum labels that wlroots expects */
    if (def->id == DRM_PROP_ID_TYPE) {
      p->count_enum_blobs = 3;
      if (p->enum_blob_ptr) {
        struct {
          uint64_t value;
          char name[32];
        } *enums = (void *)p->enum_blob_ptr;
        enums[0].value = 0;
        strcpy(enums[0].name, "Overlay");
        enums[1].value = 1;
        strcpy(enums[1].name, "Primary");
        enums[2].value = 2;
        strcpy(enums[2].name, "Cursor");
      }
    } else {
      p->count_enum_blobs = 0;
    }
    p->count_values = 0;
  } else {
    p->count_values = 0;
  }
  return 0;
}

/* ── Atomic commit engine ────────────────────────────────────────────────── */

extern struct drm_gem_object *drm_gem_find_by_handle(struct drm_device *dev,
                                                     uint32_t handle);
extern void drm_file_send_event(struct drm_file *file,
                                struct drm_event_vblank *ev,
                                struct vfs_node *node);

/*
 * Apply a single (object, property, value) triple.
 * Returns 0 on success, -1 on unknown object/property.
 */
static int atomic_apply_prop(struct drm_device *dev,
                             struct drm_mode_object *obj, uint32_t prop_id,
                             uint64_t value) {
  /* Validate the property exists in our catalogue */
  if (!drm_prop_find_def(prop_id))
    return -38; /* ENOSYS */

  switch (obj->type) {
  case DRM_MODE_OBJECT_PLANE: {
    struct drm_plane *plane = (struct drm_plane *)obj;
    switch (prop_id) {
    case DRM_PROP_ID_FB_ID: {
      /* Attach framebuffer to plane — find the linked CRTC and update it */
      struct drm_mode_object *cobj;
      list_for_each_entry(cobj, &dev->kms_objects, list) {
        if (cobj->type != DRM_MODE_OBJECT_CRTC)
          continue;
        struct drm_crtc *crtc = (struct drm_crtc *)cobj;
        if (crtc->primary == plane || crtc->cursor == plane) {
          if (value == 0) {
            crtc->fb = NULL;
          } else {
            struct drm_mode_object *fbobj;
            list_for_each_entry(fbobj, &dev->kms_objects, list) {
              if (fbobj->type == DRM_MODE_OBJECT_FB &&
                  fbobj->id == (uint32_t)value) {
                crtc->fb = (struct drm_framebuffer *)fbobj;
                break;
              }
            }
          }
          break;
        }
      }
      break;
    }
    case DRM_PROP_ID_CRTC_ID:
    case DRM_PROP_ID_SRC_X:
    case DRM_PROP_ID_SRC_Y:
    case DRM_PROP_ID_SRC_W:
    case DRM_PROP_ID_SRC_H:
    case DRM_PROP_ID_CRTC_X:
    case DRM_PROP_ID_CRTC_Y:
    case DRM_PROP_ID_CRTC_W:
    case DRM_PROP_ID_CRTC_H:
      /* Store in the object's property table */
      break;
    default:
      return -1;
    }
    break;
  }
  case DRM_MODE_OBJECT_CRTC: {
    switch (prop_id) {
    case DRM_PROP_ID_ACTIVE:
      /* ACTIVE=0 means disable CRTC; we just track it */
      break;
    case DRM_PROP_ID_MODE_ID:
      /* MODE_ID points to a blob containing drm_mode_modeinfo */
      break;
    default:
      return -1;
    }
    break;
  }
  case DRM_MODE_OBJECT_CONNECTOR: {
    switch (prop_id) {
    case DRM_PROP_ID_DPMS:
    case DRM_PROP_ID_CONNECTOR_ID:
      break;
    case DRM_PROP_ID_CRTC_ID: {
      /*
       * wlroots sets CRTC_ID on the connector during atomic modeset.
       * Link this connector to the specified CRTC so the KMS pipeline
       * is complete: connector → encoder → CRTC.
       */
      struct drm_connector *conn = (struct drm_connector *)obj;
      if (value == 0) {
        /* Disconnect: detach encoder from any CRTC */
        (void)conn;
      } else {
        /* Find the CRTC and make sure the encoder points to it */
        struct drm_mode_object *cobj;
        list_for_each_entry(cobj, &dev->kms_objects, list) {
          if (cobj->type == DRM_MODE_OBJECT_CRTC &&
              cobj->id == (uint32_t)value) {
            /* The encoder already has possible_crtcs=0x1 covering
             * this CRTC — nothing structural to change, just
             * persist the value so GETCONNECTOR reflects it. */
            break;
          }
        }
      }
      break;
    }
    default:
      return -38; /* ENOSYS */
    }
    break;
  }
  default:
    return -38; /* ENOSYS */
  }

  /* Persist the value in the object's property table */
  drm_obj_set_prop(obj, prop_id, value);
  return 0;
}

int drm_ioctl_atomic(struct vfs_node *node, struct drm_file *file,
                     struct drm_device *dev, uint64_t arg) {
  struct drm_mode_atomic *req = (struct drm_mode_atomic *)arg;

  if (!req->count_objs)
    return 0;

  uint32_t *obj_ids = (uint32_t *)req->objs_ptr;
  uint32_t *prop_cnts = (uint32_t *)req->count_props_ptr;
  uint32_t *prop_ids = (uint32_t *)req->props_ptr;
  uint64_t *prop_vals = (uint64_t *)req->prop_values_ptr;

  if (!obj_ids || !prop_cnts || !prop_ids || !prop_vals)
    return -14; /* EFAULT */

  int test_only = (req->flags & DRM_MODE_ATOMIC_TEST_ONLY) != 0;
  int nonblock = (req->flags & DRM_MODE_ATOMIC_NONBLOCK) != 0;
  (void)nonblock;

  spinlock_acquire(&dev->lock);

  uint32_t prop_offset = 0;
  for (uint32_t i = 0; i < req->count_objs; i++) {
    uint32_t obj_id = obj_ids[i];
    uint32_t num_props = prop_cnts[i];

    /* Find the object */
    struct drm_mode_object *mobj = NULL;
    struct drm_mode_object *iter;
    list_for_each_entry(iter, &dev->kms_objects, list) {
      if (iter->id == obj_id) {
        mobj = iter;
        break;
      }
    }
    if (!mobj) {
      spinlock_release(&dev->lock);
      klog_puts("[DRM] atomic: unknown object id=");
      klog_uint64(obj_id);
      klog_puts("\n");
      return -1;
    }

    for (uint32_t j = 0; j < num_props; j++) {
      uint32_t pid = prop_ids[prop_offset + j];
      uint64_t val = prop_vals[prop_offset + j];

      klog_puts("[DRM] atomic: obj=");
      klog_uint64(obj_id);
      klog_puts(" prop=");
      klog_uint64(pid);
      klog_puts(" val=");
      klog_uint64(val);
      klog_puts("\n");

      if (!test_only) {
        if (atomic_apply_prop(dev, mobj, pid, val) != 0) {
          klog_puts("[DRM] atomic: unknown prop_id=");
          klog_uint64(pid);
          klog_puts(" on obj=");
          klog_uint64(obj_id);
          klog_puts(" (ignored)\n");
          /* Non-fatal: skip unknown props for forward compat */
        }
      }
    }
    prop_offset += num_props;
  }

  /* Fire a page-flip complete event to the calling client's queue */
  if (!test_only && (req->flags & DRM_MODE_PAGE_FLIP_EVENT)) {
    struct drm_event_vblank ev = {0};
    ev.base.type = DRM_EVENT_FLIP_COMPLETE;
    ev.base.length = sizeof(struct drm_event_vblank);
    ev.user_data = req->user_data;
    spinlock_release(&dev->lock);
    drm_file_send_event(file, &ev, node);
    goto done;
  }

  spinlock_release(&dev->lock);
done:

  klog_puts("[DRM] atomic commit: ");
  klog_uint64(req->count_objs);
  klog_puts(" objects, flags=0x");
  klog_hex32(req->flags);
  klog_puts(test_only ? " (TEST_ONLY)\n" : "\n");

  return 0;
}
