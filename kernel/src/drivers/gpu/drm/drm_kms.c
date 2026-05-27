#include "../../../console/klog.h"
#include "../../../mm/heap.h"
#include "../../../lib/string.h"
#include "drm.h"

extern struct drm_gem_object *drm_gem_find_by_handle(struct drm_device *dev, uint32_t handle);

void drm_mode_object_init(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type) {
    spinlock_acquire(&dev->lock);
    obj->id = dev->next_kms_id++;
    obj->type = type;
    list_add_tail(&obj->list, &dev->kms_objects);
    spinlock_release(&dev->lock);
}

struct drm_plane *drm_plane_create(struct drm_device *dev, uint32_t possible_crtcs) {
    struct drm_plane *plane = kmalloc(sizeof(struct drm_plane));
    if (!plane) return NULL;
    memset(plane, 0, sizeof(struct drm_plane));
    
    plane->possible_crtcs = possible_crtcs;
    drm_mode_object_init(dev, &plane->base, DRM_MODE_OBJECT_PLANE);
    return plane;
}

struct drm_crtc *drm_crtc_create(struct drm_device *dev, struct drm_plane *primary) {
    struct drm_crtc *crtc = kmalloc(sizeof(struct drm_crtc));
    if (!crtc) return NULL;
    memset(crtc, 0, sizeof(struct drm_crtc));

    crtc->primary = primary;
    drm_mode_object_init(dev, &crtc->base, DRM_MODE_OBJECT_CRTC);
    return crtc;
}

struct drm_encoder *drm_encoder_create(struct drm_device *dev, uint32_t type, uint32_t possible_crtcs) {
    struct drm_encoder *enc = kmalloc(sizeof(struct drm_encoder));
    if (!enc) return NULL;
    memset(enc, 0, sizeof(struct drm_encoder));

    enc->encoder_type = type;
    enc->possible_crtcs = possible_crtcs;
    drm_mode_object_init(dev, &enc->base, DRM_MODE_OBJECT_ENCODER);
    return enc;
}

struct drm_connector *drm_connector_create(struct drm_device *dev, uint32_t type) {
    struct drm_connector *conn = kmalloc(sizeof(struct drm_connector));
    if (!conn) return NULL;
    memset(conn, 0, sizeof(struct drm_connector));

    conn->connector_type = type;
    conn->connection_status = 1; // Connected
    drm_mode_object_init(dev, &conn->base, DRM_MODE_OBJECT_CONNECTOR);
    return conn;
}

void drm_kms_init(struct drm_device *dev) {
    klog_puts("[DRM] Initializing KMS components...\n");
    // 1. Create a primary plane
    struct drm_plane *primary = drm_plane_create(dev, 0x1);
    
    // 1a. Create a cursor plane
    struct drm_plane *cursor = drm_plane_create(dev, 0x1);
    (void)cursor;

    // 2. Create a CRTC and link to primary plane
    struct drm_crtc *crtc = drm_crtc_create(dev, primary);

    // 3. Create an encoder linked to CRTC 1
    struct drm_encoder *encoder = drm_encoder_create(dev, 1 /* bits */, 0x1);

    // 4. Create a connector linked to encoder
    struct drm_connector *connector = drm_connector_create(dev, 11 /* HDMI */);
    connector->encoder = encoder;

    klog_puts("[DRM] KMS Pipeline: Plane(");
    klog_uint64(primary->base.id);
    klog_puts(") -> CRTC(");
    klog_uint64(crtc->base.id);
    klog_puts(") -> Encoder(");
    klog_uint64(encoder->base.id);
    klog_puts(") -> Connector(");
    klog_uint64(connector->base.id);
    klog_puts(")\n");
}

struct drm_framebuffer *drm_framebuffer_create(struct drm_device *dev, struct drm_mode_fb_cmd *cmd) {
    struct drm_gem_object *gem_obj = drm_gem_find_by_handle(dev, cmd->handle);
    if (!gem_obj) return NULL;

    struct drm_framebuffer *fb = kmalloc(sizeof(struct drm_framebuffer));
    if (!fb) return NULL;
    memset(fb, 0, sizeof(struct drm_framebuffer));

    fb->width = cmd->width;
    fb->height = cmd->height;
    fb->pitch = cmd->pitch;
    fb->bpp = cmd->bpp;
    fb->gem_obj = gem_obj;
    gem_obj->refcount++;

    drm_mode_object_init(dev, &fb->base, DRM_MODE_OBJECT_FB);
    return fb;
}

void drm_framebuffer_free(struct drm_device *dev, struct drm_framebuffer *fb) {
    spinlock_acquire(&dev->lock);
    list_del(&fb->base.list);
    spinlock_release(&dev->lock);

    if (fb->gem_obj) {
        fb->gem_obj->refcount--;
        // If we had a real GEM system we would check for 0 here
    }
    kfree(fb);
}
