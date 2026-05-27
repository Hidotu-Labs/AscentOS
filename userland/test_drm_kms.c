#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DRM_IOCTL_MODE_GETRESOURCES 0xC04064A0
#define DRM_IOCTL_MODE_GETCRTC      0xC06864A1
#define DRM_IOCTL_MODE_GETCONNECTOR 0xC05064A7
#define DRM_IOCTL_MODE_GETENCODER   0xC01464A6

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

int main() {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/dri/card0");
        return 1;
    }

    printf("--- AscentOS DRM KMS Resource Test ---\n");

    struct drm_mode_card_res res = {0};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        perror("GETRESOURCES (1) failed");
        return 1;
    }

    printf("KMS Resources Found:\n");
    printf("  CRTCs:      %d\n", res.count_crtcs);
    printf("  Connectors: %d\n", res.count_connectors);
    printf("  Encoders:   %d\n", res.count_encoders);
    printf("  FBs:        %d\n\n", res.count_fbs);

    uint32_t *crtc_ids = malloc(res.count_crtcs * sizeof(uint32_t));
    uint32_t *conn_ids = malloc(res.count_connectors * sizeof(uint32_t));
    uint32_t *enc_ids = malloc(res.count_encoders * sizeof(uint32_t));

    res.crtc_id_ptr = (uint64_t)crtc_ids;
    res.connector_id_ptr = (uint64_t)conn_ids;
    res.encoder_id_ptr = (uint64_t)enc_ids;

    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        perror("GETRESOURCES (2) failed");
        return 1;
    }

    // Inspect CRTCs
    for (uint32_t i = 0; i < res.count_crtcs; i++) {
        struct drm_mode_get_crtc crtc = {0};
        crtc.crtc_id = crtc_ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0) {
            printf("CRTC [ID:%u]:\n", crtc.crtc_id);
            printf("  Current Mode: %s (%ux%u @ %uHz)\n", 
                crtc.mode.name, crtc.mode.hdisplay, crtc.mode.vdisplay, crtc.mode.vrefresh);
            printf("  Valid: %s\n\n", crtc.mode_valid ? "Yes" : "No");
        } else {
            perror("GETCRTC failed");
        }
    }

    // Inspect Encoders
    for (uint32_t i = 0; i < res.count_encoders; i++) {
        struct drm_mode_get_encoder enc = {0};
        enc.encoder_id = enc_ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
            printf("Encoder [ID:%u]:\n", enc.encoder_id);
            printf("  Type: %u\n", enc.encoder_type);
            printf("  CRTC: %u\n\n", enc.crtc_id);
        } else {
            perror("GETENCODER failed");
        }
    }

    // Inspect Connectors
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector conn = {0};
        conn.connector_id = conn_ids[i];
        
        // First call to get counts
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
            perror("GETCONNECTOR (1) failed");
            continue;
        }

        struct drm_mode_modeinfo *modes = malloc(conn.count_modes * sizeof(struct drm_mode_modeinfo));
        conn.modes_ptr = (uint64_t)modes;

        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0) {
            printf("Connector [ID:%u]:\n", conn.connector_id);
            printf("  Type: %u\n", conn.connector_type);
            printf("  Status: %s\n", conn.connection == 1 ? "Connected" : "Disconnected");
            printf("  Encoder ID: %u\n", conn.encoder_id);
            printf("  Modes available: %u\n", conn.count_modes);
            for (uint32_t j = 0; j < conn.count_modes; j++) {
                printf("    - %s (%ux%u)\n", modes[j].name, modes[j].hdisplay, modes[j].vdisplay);
            }
            printf("\n");
        } else {
            perror("GETCONNECTOR (2) failed");
        }
        free(modes);
    }

    close(fd);
    return 0;
}
