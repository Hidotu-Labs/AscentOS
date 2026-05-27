#ifndef DRM_H
#define DRM_H

#include "../../../fs/vfs.h"
#include "../../../lib/list.h"
#include "../../../lib/string.h"
#include "../../../lock/spinlock.h"
#include <stddef.h>
#include <stdint.h>

#define DRM_MAJOR 226

// KMS Object types
#define DRM_MODE_OBJECT_CRTC 0xcccccccc
#define DRM_MODE_OBJECT_PLANE 0xeeeeeeee
#define DRM_MODE_OBJECT_CONNECTOR 0x000000c0
#define DRM_MODE_OBJECT_ENCODER 0xeec0ffee
#define DRM_MODE_OBJECT_FB 0xfbfbfbfb

// Standard DRM IOCTLs (simplified)
#define DRM_IOCTL_VERSION 0xC0406400
#define DRM_IOCTL_GET_CAP 0xC010640C
#define DRM_IOCTL_GEM_CREATE 0xC0106401
#define DRM_IOCTL_GEM_FREE 0x40086402
#define DRM_IOCTL_GEM_MMAP 0xC0106403

#define DRM_IOCTL_SET_MASTER 0x0000641E
#define DRM_IOCTL_DROP_MASTER 0x0000641F

#define DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0
#define DRM_IOCTL_MODE_GETCRTC 0xC06864A1
#define DRM_IOCTL_MODE_SETCRTC 0xC06864A2
#define DRM_IOCTL_MODE_GETCONNECTOR 0xC05064A7
#define DRM_IOCTL_MODE_GETENCODER 0xC01464A6
#define DRM_IOCTL_MODE_ADDFB 0xC01C64AE
#define DRM_IOCTL_MODE_RMFB 0xC00464AF
#define DRM_IOCTL_MODE_PAGE_FLIP 0x401864B0
#define DRM_IOCTL_MODE_CREATE_DUMB 0xC02064B2
#define DRM_IOCTL_MODE_MAP_DUMB 0xC01064B3
#define DRM_IOCTL_SET_CLIENT_CAP 0x4010640D
#define DRM_IOCTL_MODE_GETPROPERTY 0xC04064AA
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xC01064B5
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2

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

struct drm_mode_modeinfo {
  uint32_t clock;
  uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
  uint32_t vrefresh;
  uint32_t flags;
  uint32_t type;
  char name[32];
};

struct drm_mode_get_crtc {
  uint64_t set_connectors_ptr;
  uint32_t count_connectors;
  uint32_t crtc_id;
  uint32_t fb_id;
  uint32_t x, y;
  uint32_t gamma_size;
  uint32_t mode_valid;
  struct drm_mode_modeinfo mode;
};

struct drm_mode_crtc {
  uint64_t set_connectors_ptr;
  uint32_t count_connectors;
  uint32_t crtc_id;
  uint32_t fb_id;
  uint32_t x, y;
  uint32_t mode_valid;
  struct drm_mode_modeinfo mode;
};

struct drm_mode_fb_cmd {
  uint32_t fb_id;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t bpp;
  uint32_t depth;
  uint32_t handle;
};

struct drm_mode_crtc_page_flip {
  uint32_t crtc_id;
  uint32_t fb_id;
  uint32_t flags;
  uint32_t reserved;
  uint64_t user_data;
};

#define DRM_MODE_PAGE_FLIP_EVENT 0x01

struct drm_event {
  uint32_t type;
  uint32_t length;
};

#define DRM_EVENT_VBLANK 0x01
#define DRM_EVENT_FLIP_COMPLETE 0x02

struct drm_event_vblank {
  struct drm_event base;
  uint64_t user_data;
  uint32_t tv_sec;
  uint32_t tv_usec;
  uint32_t sequence;
  uint32_t reserved;
};

struct drm_mode_get_connector {
  uint64_t encoders_ptr;
  uint64_t modes_ptr;
  uint64_t props_ptr;
  uint64_t prop_values_ptr;
  uint32_t count_modes;
  uint32_t count_encoders;
  uint32_t count_props;
  uint32_t connector_id;
  uint32_t encoder_id;
  uint32_t connector_type;
  uint32_t connector_type_id;
  uint32_t connection;
  uint32_t mm_width, mm_height;
  uint32_t subpixel;
  uint32_t pad;
};

struct drm_mode_get_encoder {
  uint32_t encoder_id;
  uint32_t encoder_type;
  uint32_t crtc_id;
  uint32_t possible_crtcs;
  uint32_t possible_clones;
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

struct drm_set_client_cap {
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

#include "../../../sched/sched.h"
#include "../../../sched/wait.h"

struct drm_device {
  const char *name;
  uint32_t minor;
  spinlock_t lock;
  struct list_head gem_objects;
  struct list_head kms_objects;
  struct list_head event_queue;
  wait_queue_t event_wq;
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
  struct drm_framebuffer *fb;
};

struct drm_framebuffer {
  struct drm_mode_object base;
  uint32_t width, height;
  uint32_t pitch, bpp;
  struct drm_gem_object *gem_obj;
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

struct drm_pending_event {
  struct drm_event_vblank event;
  struct list_head list;
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
