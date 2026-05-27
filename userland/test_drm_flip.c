#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0
#define DRM_IOCTL_MODE_GETCRTC      0xC06864A1
#define DRM_IOCTL_MODE_SETCRTC      0xC06864A2
#define DRM_IOCTL_MODE_GETCONNECTOR 0xC05064A7
#define DRM_IOCTL_MODE_ADDFB        0xC01C64AE
#define DRM_IOCTL_MODE_RMFB         0xC00464AF
#define DRM_IOCTL_MODE_PAGE_FLIP    0x401864B0
#define DRM_IOCTL_MODE_CREATE_DUMB  0xC02064B2
#define DRM_IOCTL_MODE_MAP_DUMB     0xC01064B3

struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id, width, height, pitch, bpp, depth, handle;
};

struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle, pad;
    uint64_t offset;
};

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id, fb_id, flags, reserved;
    uint64_t user_data;
};

#define DRM_MODE_PAGE_FLIP_EVENT 0x01

struct drm_event {
    uint32_t type, length;
};

#define DRM_EVENT_FLIP_COMPLETE  0x02

struct drm_event_vblank {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec, tv_usec, sequence, reserved;
};

int main() {
    int fd = open("/dev/dri/card0", O_RDONLY); // Actually O_RDWR is better
    close(fd);
    fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/dri/card0");
        return 1;
    }

    printf("--- AscentOS DRM Mode-Setting & Flip Test ---\n");

    // 1. Discovery
    struct drm_mode_card_res res = {0};
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    uint32_t *crtc_ids = malloc(res.count_crtcs * 4);
    uint32_t *conn_ids = malloc(res.count_connectors * 4);
    res.crtc_id_ptr = (uintptr_t)crtc_ids;
    res.connector_id_ptr = (uintptr_t)conn_ids;
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);

    uint32_t crtc_id = crtc_ids[0];
    uint32_t conn_id = conn_ids[0];
    printf("Using CRTC:%u and Connector:%u\n", crtc_id, conn_id);

    // 2. Create Dumb Buffer (Purple)
    struct drm_mode_create_dumb cre = {0};
    cre.width = 1280; cre.height = 800; cre.bpp = 32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cre) < 0) {
        perror("CREATE_DUMB failed");
        return 1;
    }
    printf("Created DUMB buffer: handle=%u, size=%llu\n", cre.handle, (unsigned long long)cre.size);

    struct drm_mode_map_dumb map = {0};
    map.handle = cre.handle;
    ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    uint32_t *ptr = mmap(0, cre.size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, map.offset);
    for (uint32_t i = 0; i < cre.size/4; i++) ptr[i] = 0xFF00FF; // Purple

    // 3. Add FB
    struct drm_mode_fb_cmd fb_cmd = {0};
    fb_cmd.width = 1280; fb_cmd.height = 800; fb_cmd.handle = cre.handle;
    fb_cmd.bpp = 32; fb_cmd.pitch = cre.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd) < 0) {
        perror("ADDFB failed");
        return 1;
    }
    printf("Registered FB: id=%u\n", fb_cmd.fb_id);

    // 4. Set CRTC
    struct drm_mode_crtc crtc_set = {0};
    crtc_set.crtc_id = crtc_id;
    crtc_set.fb_id = fb_cmd.fb_id;
    crtc_set.set_connectors_ptr = (uintptr_t)&conn_id;
    crtc_set.count_connectors = 1;
    crtc_set.mode.hdisplay = 1280; crtc_set.mode.vdisplay = 800;
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc_set) < 0) {
        perror("SETCRTC failed");
        return 1;
    }
    printf("SETCRTC success! Screen should be Purple.\n");
    sleep(1);

    // 5. Page Flip (Change to Blue)
    for (uint32_t i = 0; i < cre.size/4; i++) ptr[i] = 0x0000FF; // Blue
    struct drm_mode_crtc_page_flip flip = {0};
    flip.crtc_id = crtc_id;
    flip.fb_id = fb_cmd.fb_id;
    flip.flags = DRM_MODE_PAGE_FLIP_EVENT;
    flip.user_data = 0x12345678;
    if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0) {
        perror("PAGE_FLIP failed");
        return 1;
    }
    printf("PAGE_FLIP requested, waiting for event...\n");

    // 6. Read Event
    struct drm_event_vblank ev;
    int len = read(fd, &ev, sizeof(ev));
    if (len > 0) {
        if (ev.base.type == DRM_EVENT_FLIP_COMPLETE) {
            printf("Success! Received FLIP_COMPLETE event.\n");
            printf("User data returned: 0x%llx\n", (unsigned long long)ev.user_data);
        } else {
            printf("Received unknown event type: %u\n", ev.base.type);
        }
    } else {
        perror("read event failed");
    }

    printf("Test complete. Cleaning up...\n");
    ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_cmd.fb_id);
    close(fd);
    return 0;
}
