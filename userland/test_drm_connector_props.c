/*
 * test_drm_connector_props.c
 *
 * Validates the two fixes needed for wlroots/tinywl to start:
 *
 *  Fix 1 — Connector exposes properties via GETCONNECTOR
 *           (wlroots needs CRTC_ID property ID on the connector)
 *
 *  Fix 2 — Atomic commit accepts CRTC_ID on a connector object
 *           (wlroots sets connector.CRTC_ID during modeset)
 *
 * Also validates the full wlroots startup ioctl sequence end-to-end.
 */
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── IOCTL numbers ─────────────────────────────────────────────────────── */
#define DRM_IOCTL_VERSION               0xC0406400
#define DRM_IOCTL_GET_CAP               0xC010640C
#define DRM_IOCTL_SET_CLIENT_CAP        0x4010640D
#define DRM_IOCTL_SET_MASTER            0x0000641E
#define DRM_IOCTL_MODE_GETRESOURCES     0xC04064A0
#define DRM_IOCTL_MODE_GETCRTC          0xC06864A1
#define DRM_IOCTL_MODE_GETCONNECTOR     0xC05064A7
#define DRM_IOCTL_MODE_GETENCODER       0xC01464A6
#define DRM_IOCTL_MODE_GETPROPERTY      0xC04064AA
#define DRM_IOCTL_MODE_ADDFB2           0xC04464B8
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES 0xC01064B9
#define DRM_IOCTL_MODE_ATOMIC           0xC03C64BC
#define DRM_IOCTL_MODE_CREATEPROPBLOB   0xC01064BD
#define DRM_IOCTL_MODE_DESTROYPROPBLOB  0xC00464BE
#define DRM_IOCTL_MODE_GETPLANERESOURCES 0xC01064B5
#define DRM_IOCTL_MODE_CREATE_DUMB      0xC02064B2
#define DRM_IOCTL_MODE_MAP_DUMB         0xC01064B3

/* ── Caps / flags ──────────────────────────────────────────────────────── */
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 2
#define DRM_CLIENT_CAP_ATOMIC           3
#define DRM_MODE_ATOMIC_TEST_ONLY       0x0100
#define DRM_MODE_ATOMIC_ALLOW_MODESET   0x0400
#define DRM_MODE_PAGE_FLIP_EVENT        0x01

/* ── Property IDs ──────────────────────────────────────────────────────── */
#define DRM_PROP_ID_CRTC_ID      1
#define DRM_PROP_ID_FB_ID        2
#define DRM_PROP_ID_SRC_W        5
#define DRM_PROP_ID_SRC_H        6
#define DRM_PROP_ID_CRTC_W       9
#define DRM_PROP_ID_CRTC_H       10
#define DRM_PROP_ID_ACTIVE       11
#define DRM_PROP_ID_MODE_ID      12
#define DRM_PROP_ID_DPMS         13
#define DRM_PROP_ID_CONNECTOR_ID 14

/* ── Structs ───────────────────────────────────────────────────────────── */
struct drm_set_client_cap { uint64_t capability; uint64_t value; };
struct drm_get_cap        { uint64_t capability; uint64_t value; };
struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type; char name[32];
};
struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_encoders, count_props;
    uint32_t connector_id, encoder_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};
struct drm_mode_get_property {
    uint64_t values_ptr, enum_blob_ptr;
    uint32_t prop_id, flags; char name[32];
    uint32_t count_values, count_enum_blobs;
};
struct drm_mode_obj_get_properties {
    uint64_t props_ptr, prop_values_ptr;
    uint32_t count_props, obj_id, obj_type, pad;
};
struct drm_mode_create_blob { uint64_t data; uint32_t length; uint32_t blob_id; };
struct drm_mode_destroy_blob { uint32_t blob_id; };
struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch; uint64_t size;
};
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_fb_cmd2 {
    uint32_t fb_id, width, height, pixel_format, flags;
    uint32_t handles[4], pitches[4], offsets[4]; uint64_t modifier[4];
};
struct drm_mode_atomic {
    uint32_t flags, count_objs;
    uint64_t objs_ptr, count_props_ptr, props_ptr, prop_values_ptr;
    uint64_t reserved, user_data;
};
struct drm_event        { uint32_t type, length; };
struct drm_event_vblank {
    struct drm_event base; uint64_t user_data;
    uint32_t tv_sec, tv_usec, sequence, reserved;
};
#define DRM_EVENT_FLIP_COMPLETE 0x02
struct plane_res { uint64_t plane_id_ptr; uint32_t count_planes; };

/* ── Helpers ───────────────────────────────────────────────────────────── */
#define PASS(fmt, ...) printf("  [PASS] " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) do { printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); failures++; } while(0)
#define SECTION(s)     printf("\n=== %s ===\n", s)
static int failures = 0;

static int open_drm(void) {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) return -1;
    struct drm_set_client_cap cap;
    cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES; cap.value = 1;
    ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    cap.capability = DRM_CLIENT_CAP_ATOMIC; cap.value = 1;
    ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &cap);
    ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
    return fd;
}

int main(void) {
    printf("=== AscentOS DRM Connector Properties + wlroots Modeset Test ===\n");

    int fd = open_drm();
    if (fd < 0) { perror("open /dev/dri/card0"); return 1; }
    PASS("Opened /dev/dri/card0 fd=%d", fd);

    /* ── Discover resources ────────────────────────────────────────────── */
    struct drm_mode_card_res res = {0};
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
    uint32_t crtc_ids[8]={0}, conn_ids[8]={0};
    res.crtc_id_ptr      = (uintptr_t)crtc_ids;
    res.connector_id_ptr = (uintptr_t)conn_ids;
    ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);

    struct plane_res pr = {0};
    ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
    uint32_t plane_ids[8] = {0};
    pr.plane_id_ptr = (uintptr_t)plane_ids;
    ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);

    if (res.count_crtcs == 0 || res.count_connectors == 0 || pr.count_planes == 0) {
        FAIL("No KMS resources found"); close(fd); return 1;
    }
    uint32_t crtc_id  = crtc_ids[0];
    uint32_t conn_id  = conn_ids[0];
    uint32_t plane_id = plane_ids[0];
    printf("  Using CRTC=%u  Connector=%u  Plane=%u\n", crtc_id, conn_id, plane_id);

    /* ══════════════════════════════════════════════════════════════════════
     * FIX 1: Connector must expose properties via GETCONNECTOR
     * wlroots calls GETCONNECTOR twice: first to get counts, then to fill.
     * ══════════════════════════════════════════════════════════════════════ */
    SECTION("Fix 1: GETCONNECTOR exposes properties");

    /* First call — get counts */
    struct drm_mode_get_connector conn1 = {0};
    conn1.connector_id = conn_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn1) < 0) {
        FAIL("GETCONNECTOR first call failed"); goto cleanup;
    }
    printf("  count_props=%u  count_modes=%u  count_encoders=%u\n",
           conn1.count_props, conn1.count_modes, conn1.count_encoders);

    if (conn1.count_props == 0) {
        FAIL("Connector has 0 properties — wlroots will not find CRTC_ID");
        goto cleanup;
    }
    PASS("Connector reports %u properties", conn1.count_props);

    /* Second call — fill arrays */
    uint32_t *prop_ids  = malloc(conn1.count_props * 4);
    uint64_t *prop_vals = malloc(conn1.count_props * 8);
    struct drm_mode_modeinfo *modes = malloc(conn1.count_modes * sizeof(*modes));
    uint32_t *enc_ids = malloc((conn1.count_encoders + 1) * 4);

    struct drm_mode_get_connector conn2 = {0};
    conn2.connector_id    = conn_id;
    conn2.props_ptr       = (uintptr_t)prop_ids;
    conn2.prop_values_ptr = (uintptr_t)prop_vals;
    conn2.modes_ptr       = (uintptr_t)modes;
    conn2.encoders_ptr    = (uintptr_t)enc_ids;
    conn2.count_props     = conn1.count_props;
    conn2.count_modes     = conn1.count_modes;
    conn2.count_encoders  = conn1.count_encoders;

    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn2) < 0) {
        FAIL("GETCONNECTOR second call failed"); goto cleanup;
    }

    /* Verify CRTC_ID property is present */
    int found_crtc_id_prop = 0;
    int found_dpms_prop    = 0;
    printf("  Connector properties:\n");
    for (uint32_t i = 0; i < conn2.count_props; i++) {
        /* Query the property name */
        struct drm_mode_get_property gp = {0};
        gp.prop_id = prop_ids[i];
        ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp);
        printf("    prop_id=%-2u name='%s' value=%llu\n",
               prop_ids[i], gp.name, (unsigned long long)prop_vals[i]);
        if (strcmp(gp.name, "CRTC_ID") == 0) found_crtc_id_prop = 1;
        if (strcmp(gp.name, "DPMS")    == 0) found_dpms_prop    = 1;
    }

    if (found_crtc_id_prop)
        PASS("CRTC_ID property found on connector (wlroots can discover it)");
    else
        FAIL("CRTC_ID property NOT found on connector — wlroots modeset will fail");

    if (found_dpms_prop)
        PASS("DPMS property found on connector");
    else
        FAIL("DPMS property missing from connector");

    /* Verify mode info is sane */
    if (conn2.count_modes > 0 && modes[0].hdisplay > 0 && modes[0].vdisplay > 0)
        PASS("Mode: %s %ux%u@%uHz type=0x%x",
             modes[0].name, modes[0].hdisplay, modes[0].vdisplay,
             modes[0].vrefresh, modes[0].type);
    else
        FAIL("No valid mode returned from connector");

    free(prop_ids); free(prop_vals); free(modes); free(enc_ids);

    /* ══════════════════════════════════════════════════════════════════════
     * FIX 2: Atomic commit must accept CRTC_ID on connector object
     * wlroots sets connector.CRTC_ID during the modeset atomic commit.
     * ══════════════════════════════════════════════════════════════════════ */
    SECTION("Fix 2: Atomic commit accepts CRTC_ID on connector");

    /* Create a dumb buffer + FB for the commit */
    struct drm_mode_create_dumb cd = {0};
    cd.width=1280; cd.height=800; cd.bpp=32;
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
        FAIL("CREATE_DUMB failed"); goto cleanup;
    }

    struct drm_mode_fb_cmd2 fb2 = {0};
    fb2.width=1280; fb2.height=800; fb2.pixel_format=0x34325258;
    fb2.handles[0]=cd.handle; fb2.pitches[0]=cd.pitch;
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb2) < 0) {
        FAIL("ADDFB2 failed"); goto cleanup;
    }
    uint32_t fb_id = fb2.fb_id;
    PASS("Created FB id=%u for modeset test", fb_id);

    /* Create MODE_ID blob — exactly what wlroots does */
    struct drm_mode_modeinfo mode_blob = {0};
    mode_blob.clock=60000; mode_blob.hdisplay=1280; mode_blob.vdisplay=800;
    mode_blob.vrefresh=60; mode_blob.flags=0; mode_blob.type=0x48;
    strcpy(mode_blob.name, "1280x800");
    struct drm_mode_create_blob cb = {0};
    cb.data = (uintptr_t)&mode_blob; cb.length = sizeof(mode_blob);
    if (ioctl(fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &cb) < 0) {
        FAIL("CREATEPROPBLOB failed"); goto cleanup;
    }
    uint32_t mode_blob_id = cb.blob_id;
    PASS("Created MODE_ID blob id=%u", mode_blob_id);

    /*
     * wlroots atomic modeset commit sets:
     *   CRTC:      ACTIVE=1, MODE_ID=<blob>
     *   Connector: CRTC_ID=<crtc>
     *   Plane:     FB_ID=<fb>, CRTC_ID=<crtc>, SRC_W/H, CRTC_W/H
     *
     * 3 objects, property counts: crtc=2, connector=1, plane=6
     */
    uint32_t obj_ids[3]   = { crtc_id, conn_id, plane_id };
    uint32_t prop_cnts[3] = { 2, 1, 6 };

    uint32_t all_prop_ids[9] = {
        /* CRTC (2) */
        DRM_PROP_ID_ACTIVE,  DRM_PROP_ID_MODE_ID,
        /* Connector (1) */
        DRM_PROP_ID_CRTC_ID,
        /* Plane (6) */
        DRM_PROP_ID_FB_ID,   DRM_PROP_ID_CRTC_ID,
        DRM_PROP_ID_SRC_W,   DRM_PROP_ID_SRC_H,
        DRM_PROP_ID_CRTC_W,  DRM_PROP_ID_CRTC_H,
    };
    uint64_t all_prop_vals[9] = {
        /* CRTC */
        1, mode_blob_id,
        /* Connector */
        crtc_id,
        /* Plane */
        fb_id, crtc_id,
        1280 << 16, 800 << 16,
        1280, 800,
    };

    struct drm_mode_atomic atomic = {0};
    atomic.flags           = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
    atomic.count_objs      = 3;
    atomic.objs_ptr        = (uintptr_t)obj_ids;
    atomic.count_props_ptr = (uintptr_t)prop_cnts;
    atomic.props_ptr       = (uintptr_t)all_prop_ids;
    atomic.prop_values_ptr = (uintptr_t)all_prop_vals;

    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == 0)
        PASS("TEST_ONLY modeset (CRTC+Connector+Plane) accepted");
    else
        FAIL("TEST_ONLY modeset rejected — connector CRTC_ID not handled");

    /* Live commit — the real wlroots path */
    atomic.flags    = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_ALLOW_MODESET;
    atomic.user_data = 0xC0DE0000ULL;

    if (ioctl(fd, DRM_IOCTL_MODE_ATOMIC, &atomic) == 0) {
        PASS("Live modeset commit accepted (CRTC+Connector+Plane)");

        /* Read the flip event */
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        if (poll(&pfd, 1, 2000) > 0) {
            struct drm_event_vblank ev;
            int len = read(fd, &ev, sizeof(ev));
            if (len > 0 && ev.base.type == DRM_EVENT_FLIP_COMPLETE)
                PASS("FLIP_COMPLETE event received after modeset");
            else
                FAIL("Unexpected event type=%u", ev.base.type);
        } else {
            FAIL("poll timed out waiting for FLIP_COMPLETE after modeset");
        }
    } else {
        FAIL("Live modeset commit failed");
    }

    /* ── Verify connector CRTC_ID was persisted ────────────────────────── */
    SECTION("Verify connector CRTC_ID persisted after atomic commit");
    {
        struct drm_mode_obj_get_properties req = {0};
        req.obj_id = conn_id;
        ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req);
        uint32_t cnt = req.count_props;
        uint32_t *pids  = malloc(cnt * 4 + 4);
        uint64_t *pvals = malloc(cnt * 8 + 8);
        req.props_ptr       = (uintptr_t)pids;
        req.prop_values_ptr = (uintptr_t)pvals;
        ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req);

        int found = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            if (pids[i] == DRM_PROP_ID_CRTC_ID && pvals[i] == crtc_id) {
                found = 1; break;
            }
        }
        if (found)
            PASS("Connector CRTC_ID=%u persisted correctly", crtc_id);
        else
            FAIL("Connector CRTC_ID not updated after atomic commit");

        free(pids); free(pvals);
    }

    /* ── Simulate full wlroots startup sequence ────────────────────────── */
    SECTION("Full wlroots startup ioctl sequence simulation");
    {
        /* wlroots queries OBJ_GETPROPERTIES on every object after modeset */
        int all_ok = 1;
        for (uint32_t i = 0; i < res.count_crtcs; i++) {
            struct drm_mode_obj_get_properties req = {0};
            req.obj_id = crtc_ids[i];
            ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req);
            if (req.count_props == 0) { all_ok = 0; break; }
        }
        for (uint32_t i = 0; i < res.count_connectors; i++) {
            struct drm_mode_obj_get_properties req = {0};
            req.obj_id = conn_ids[i];
            ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req);
            if (req.count_props == 0) { all_ok = 0; break; }
        }
        for (uint32_t i = 0; i < pr.count_planes; i++) {
            struct drm_mode_obj_get_properties req = {0};
            req.obj_id = plane_ids[i];
            ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &req);
            if (req.count_props == 0) { all_ok = 0; break; }
        }
        if (all_ok)
            PASS("All KMS objects have properties (wlroots enumeration OK)");
        else
            FAIL("Some KMS objects have 0 properties");

        /* wlroots queries GETPROPERTY for every property ID it finds */
        /* Spot-check the critical ones */
        const char *required[] = { "CRTC_ID", "FB_ID", "ACTIVE", "MODE_ID",
                                    "DPMS", "SRC_W", "SRC_H", NULL };
        uint32_t check_ids[]   = { DRM_PROP_ID_CRTC_ID, DRM_PROP_ID_FB_ID,
                                    DRM_PROP_ID_ACTIVE, DRM_PROP_ID_MODE_ID,
                                    DRM_PROP_ID_DPMS, DRM_PROP_ID_SRC_W,
                                    DRM_PROP_ID_SRC_H };
        for (int i = 0; required[i]; i++) {
            struct drm_mode_get_property gp = {0};
            gp.prop_id = check_ids[i];
            ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp);
            if (strcmp(gp.name, required[i]) == 0)
                PASS("GETPROPERTY id=%-2u name='%s' OK", check_ids[i], gp.name);
            else
                FAIL("GETPROPERTY id=%-2u expected '%s' got '%s'",
                     check_ids[i], required[i], gp.name);
        }
    }

    /* Cleanup */
    {
        struct drm_mode_destroy_blob db = { .blob_id = mode_blob_id };
        ioctl(fd, DRM_IOCTL_MODE_DESTROYPROPBLOB, &db);
    }

cleanup:
    close(fd);
    printf("\n=== Summary: %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
