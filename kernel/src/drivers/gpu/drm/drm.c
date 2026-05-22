#include "drm.h"
#include "../../../fs/ramfs.h"
#include "../../../mm/heap.h"
#include "../../../lib/string.h"
#include "../../../mm/vmm.h"
#include "../../../console/klog.h"
#include "../../../fb/framebuffer.h"

struct drm_device global_drm_dev;

extern struct drm_gem_object *drm_gem_object_create(struct drm_device *dev, size_t size);
extern void drm_gem_object_free(struct drm_device *dev, struct drm_gem_object *obj);
extern struct drm_gem_object *drm_gem_find_by_handle(struct drm_device *dev, uint32_t handle);
extern void drm_kms_init(struct drm_device *dev);

static int drm_ioctl(struct vfs_node *node, uint32_t request, uint64_t arg) {
    struct drm_device *dev = (struct drm_device *)node->device;
    if (!dev) return -1;

    switch (request) {
        case DRM_IOCTL_VERSION: {
            struct drm_version *v = (struct drm_version *)arg;
            v->version_major = 1;
            v->version_minor = 0;
            v->version_patchlevel = 0;
            if (v->name) strncpy(v->name, "AscentOS DRM", v->name_len);
            if (v->date) strncpy(v->date, "20260521", v->date_len);
            if (v->desc) strncpy(v->desc, "AscentOS Graphics Subsystem", v->desc_len);
            return 0;
        }
        case DRM_IOCTL_GET_CAP: {
            struct drm_get_cap *cap = (struct drm_get_cap *)arg;
            if (cap->capability == 1 /* DUMB_BUFFER */) {
                cap->value = 1;
            } else {
                cap->value = 0;
            }
            return 0;
        }
        case DRM_IOCTL_GEM_CREATE: {
            struct drm_gem_create *c = (struct drm_gem_create *)arg;
            struct drm_gem_object *obj = drm_gem_object_create(dev, c->size);
            if (!obj) return -1;
            c->handle = obj->handle;
            return 0;
        }
        case DRM_IOCTL_GEM_FREE: {
            struct drm_gem_free *f = (struct drm_gem_free *)arg;
            struct drm_gem_object *obj = drm_gem_find_by_handle(dev, f->handle);
            if (!obj) return -1;
            drm_gem_object_free(dev, obj);
            return 0;
        }
        case DRM_IOCTL_MODE_GETRESOURCES: {
            struct drm_mode_card_res *res = (struct drm_mode_card_res *)arg;
            res->count_fbs = 0;
            res->count_crtcs = 0;
            res->count_connectors = 0;
            res->count_encoders = 0;

            struct drm_mode_object *mobj;
            spinlock_acquire(&dev->lock);
            list_for_each_entry(mobj, &dev->kms_objects, list) {
                if (mobj->type == DRM_MODE_OBJECT_CRTC) {
                    if (res->crtc_id_ptr && res->count_crtcs < 1) {
                        ((uint32_t *)res->crtc_id_ptr)[res->count_crtcs] = mobj->id;
                    }
                    res->count_crtcs++;
                } else if (mobj->type == DRM_MODE_OBJECT_CONNECTOR) {
                    if (res->connector_id_ptr && res->count_connectors < 1) {
                         ((uint32_t *)res->connector_id_ptr)[res->count_connectors] = mobj->id;
                    }
                    res->count_connectors++;
                } else if (mobj->type == DRM_MODE_OBJECT_ENCODER) {
                    res->count_encoders++;
                }
            }
            spinlock_release(&dev->lock);
            res->min_width = 0; res->max_width = 4096;
            res->min_height = 0; res->max_height = 4096;
            return 0;
        }
        case DRM_IOCTL_MODE_CREATE_DUMB: {
            struct drm_mode_create_dumb *c = (struct drm_mode_create_dumb *)arg;
            c->pitch = c->width * (c->bpp / 8);
            c->size = (uint64_t)c->pitch * c->height;
            struct drm_gem_object *obj = drm_gem_object_create(dev, c->size);
            if (!obj) return -1;
            c->handle = obj->handle;
            return 0;
        }
        case DRM_IOCTL_MODE_MAP_DUMB: {
            struct drm_mode_map_dumb *m = (struct drm_mode_map_dumb *)arg;
            struct drm_gem_object *obj = drm_gem_find_by_handle(dev, m->handle);
            if (!obj) return -1;
            m->offset = obj->phys_addr | 0x1000000000000000ULL; 
            return 0;
        }
        default:
            return -1;
    }
}

static uint64_t drm_vfs_mmap(vfs_node_t *node, uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t offset) {
    (void)node; (void)flags; (void)prot;

    if ((offset & 0x1000000000000000ULL) == 0) return (uint64_t)-1;
    
    uint64_t phys = offset & ~0x1000000000000000ULL;
    
    uint64_t vaddr = addr;
    if (vaddr == 0) {
        extern uint64_t mm_alloc_mmap_region(uint64_t length);
        vaddr = mm_alloc_mmap_region(length);
        
        // Fallback for kernel threads during boot testing
        if (vaddr == 0) {
            vaddr = 0x500000000000ULL + (offset & 0xFFFFFFF);
        }
    }
    if (vaddr == 0) return (uint64_t)-1;

    uint64_t page_flags = PAGE_FLAG_PRESENT | PAGE_FLAG_USER | PAGE_FLAG_RW;
    page_flags |= (PAGE_FLAG_PWT | PAGE_FLAG_PCD | PAGE_FLAG_PAT);

    uint32_t num_pages = (length + 4095) / 4096;
    for (uint32_t i = 0; i < num_pages; i++) {
        vmm_map_page(vmm_get_active_pml4(), vaddr + i * 4096, phys + i * 4096, page_flags);
    }

    return vaddr;
}

void drm_init(void) {
    memset(&global_drm_dev, 0, sizeof(struct drm_device));
    global_drm_dev.name = "card0";
    global_drm_dev.next_gem_handle = 1;
    global_drm_dev.next_kms_id = 1000;
    spinlock_init(&global_drm_dev.lock);
    INIT_LIST_HEAD(&global_drm_dev.gem_objects);
    INIT_LIST_HEAD(&global_drm_dev.kms_objects);

    drm_kms_init(&global_drm_dev);

    // Phase 4: Bridge Hardware Framebuffer
    void *fb_base = fb_get_base();
    if (fb_base) {
        uint64_t fb_phys = vmm_virt_to_phys(vmm_get_active_pml4(), (uint64_t)fb_base);
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
    if (!dev_dir) return;

    vfs_mkdir(dev_dir, "dri", 0755);
    vfs_node_t *dri_dir = vfs_resolve_path("/dev/dri");
    if (!dri_dir) return;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) return;

    vfs_node_init(node);
    strcpy(node->name, "card0");
    node->flags = FS_CHARDEV | FS_PERSISTENT;
    node->mask = 0666;
    node->device = &global_drm_dev;
    node->ioctl = drm_ioctl;
    node->mmap = drm_vfs_mmap;

    ramfs_mount_node(dri_dir, node);
    klog_puts("[DRM] Registered /dev/dri/card0\n");
}
