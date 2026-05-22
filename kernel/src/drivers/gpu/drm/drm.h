#ifndef DRM_H
#define DRM_H

#include "../../../lib/list.h"
#include "../../../lib/string.h"
#include "../../../fs/vfs.h"
#include "../../../lock/spinlock.h"
#include <stdint.h>
#include <stddef.h>

#define DRM_MAJOR 226

// KMS Object types
#define DRM_MODE_OBJECT_CRTC      0xcccccccc
#define DRM_MODE_OBJECT_PLANE     0xeeeeeeee
#define DRM_MODE_OBJECT_CONNECTOR 0x000000c0
#define DRM_MODE_OBJECT_ENCODER   0xeec0ffee

// Standard DRM IOCTLs (simplified)
#define DRM_IOCTL_VERSION      0xC0406400
#define DRM_IOCTL_GET_CAP      0xC010640C
#define DRM_IOCTL_GEM_CREATE   0xC0106401
#define DRM_IOCTL_GEM_FREE     0x40086402
#define DRM_IOCTL_GEM_MMAP     0xC0106403

#define DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0
#define DRM_IOCTL_MODE_GETCRTC      0xC06864A1
#define DRM_IOCTL_MODE_GETCONNECTOR 0xC05064A7
#define DRM_IOCTL_MODE_CREATE_DUMB  0xC02064B2
#define DRM_IOCTL_MODE_MAP_DUMB     0xC01064B3
#define DRM_IOCTL_MODE_ADDFB        0xC01C64AE

struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
};

struct drm_mode_get_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct {
        uint32_t clock;
        uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
        uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
        uint32_t vrefresh;
        uint32_t flags;
        uint32_t type;
        char name[32];
    } mode;
};

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_gem_create {
    uint64_t size;
    uint32_t handle;
    uint32_t pad;
};

struct drm_gem_free {
    uint32_t handle;
    uint32_t pad;
};

struct drm_gem_mmap {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
    uint64_t addr;
};

struct drm_device {
    const char *name;
    uint32_t minor;
    spinlock_t lock;
    struct list_head gem_objects;
    struct list_head kms_objects;
    uint32_t next_gem_handle;
    uint32_t next_kms_id;
};

struct drm_mode_object {
    uint32_t id;
    uint32_t type;
    struct list_head list;
};

struct drm_plane {
    struct drm_mode_object base;
    uint32_t possible_crtcs;
    uint32_t formats[8];
    int format_count;
};

struct drm_crtc {
    struct drm_mode_object base;
    struct drm_plane *primary;
    struct drm_plane *cursor;
};

struct drm_encoder {
    struct drm_mode_object base;
    uint32_t possible_crtcs;
    uint32_t encoder_type;
};

struct drm_connector {
    struct drm_mode_object base;
    uint32_t connector_type;
    uint32_t connection_status;
    struct drm_encoder *encoder;
};

struct drm_file {
    struct drm_device *dev;
    struct list_head gem_handles; // Per-file GEM handle table
    spinlock_t lock;
};

void drm_init(void);
void drm_register_vfs(void);

// GEM internals
struct drm_gem_object {
    uint32_t handle;
    size_t size;
    uint64_t phys_addr;
    void *virt_addr;
    struct list_head list;
    struct list_head file_list;
    int refcount;
};

#endif
