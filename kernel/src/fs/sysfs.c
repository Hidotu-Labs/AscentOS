#include "vfs.h"
#include "../console/klog.h"
#include "ramfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"

void sysfs_init(void) {
    if (!fs_root) return;

    // Create /sys
    vfs_mkdir(fs_root, "sys", 0755);
    vfs_node_t *sys_root = vfs_resolve_path("/sys");
    if (!sys_root) return;

    // Transform /sys into a ramfs directory
    ramfs_mount_on(sys_root);

    // Create /sys/class/drm/card0
    vfs_mkdir(sys_root, "class", 0755);
    vfs_node_t *class_dir = vfs_resolve_path("/sys/class");
    if (!class_dir) return;

    vfs_mkdir(class_dir, "drm", 0755);
    vfs_node_t *drm_dir = vfs_resolve_path("/sys/class/drm");
    if (!drm_dir) return;

    vfs_mkdir(drm_dir, "card0", 0755);
    vfs_node_t *card0_dir = vfs_resolve_path("/sys/class/drm/card0");
    if (!card0_dir) return;

    // Create /sys/class/drm/card0/dev (containing "226:0\n")
    vfs_create(card0_dir, "dev", 0444);
    vfs_node_t *dev_file = vfs_resolve_path("/sys/class/drm/card0/dev");
    if (dev_file) {
        const char *dev_str = "226:0\n";
        vfs_write(dev_file, 0, strlen(dev_str), (uint8_t *)dev_str);
    }

    // Create /sys/class/drm/card0/uevent
    vfs_create(card0_dir, "uevent", 0444);
    vfs_node_t *uevent_file = vfs_resolve_path("/sys/class/drm/card0/uevent");
    if (uevent_file) {
        const char *uevent_str = "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\nDEVTYPE=drm_minor\nSUBSYSTEM=drm\n";
        vfs_write(uevent_file, 0, strlen(uevent_str), (uint8_t *)uevent_str);
    }

    // Create /sys/dev/char/226:0 (symlink to ../../class/drm/card0)
    vfs_mkdir(sys_root, "dev", 0755);
    vfs_node_t *dev_dir = vfs_resolve_path("/sys/dev");
    if (!dev_dir) return;
    
    vfs_mkdir(dev_dir, "char", 0755);
    vfs_node_t *char_dir = vfs_resolve_path("/sys/dev/char");
    if (!char_dir) return;

    // For now, we mock a symlink with a directory or dummy file since real symlinks
    // might be complicated in this staged ramfs. udev just needs the path to exist.
    vfs_mkdir(char_dir, "226:0", 0755);
    vfs_node_t *dev_char_link = vfs_resolve_path("/sys/dev/char/226:0");
    if (dev_char_link) {
        ramfs_mount_on(dev_char_link);
        // Add a 'uevent' file here too
        vfs_create(dev_char_link, "uevent", 0444);
        vfs_node_t *uevent_file2 = vfs_resolve_path("/sys/dev/char/226:0/uevent");
        if (uevent_file2) {
             const char *uevent_str = "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\nDEVTYPE=drm_minor\n";
             vfs_write(uevent_file2, 0, strlen(uevent_str), (uint8_t *)uevent_str);
        }
    }

    // Create /sys/class/input/event0
    vfs_mkdir(class_dir, "input", 0755);
    vfs_node_t *input_dir = vfs_resolve_path("/sys/class/input");
    if (input_dir) {
        vfs_mkdir(input_dir, "event0", 0755);
        vfs_node_t *event0_dir = vfs_resolve_path("/sys/class/input/event0");
        if (event0_dir) {
            vfs_create(event0_dir, "dev", 0444);
            vfs_node_t *input_dev = vfs_resolve_path("/sys/class/input/event0/dev");
            if (input_dev) {
                const char *input_str = "13:64\n"; // Standard for /dev/input/event0
                vfs_write(input_dev, 0, strlen(input_str), (uint8_t *)input_str);
            }
        }
    }

    // Create /sys/bus/pci/devices (minimal)
    vfs_mkdir(sys_root, "bus", 0755);
    vfs_node_t *bus_dir = vfs_resolve_path("/sys/bus");
    if (bus_dir) {
        vfs_mkdir(bus_dir, "pci", 0755);
        vfs_node_t *pci_dir = vfs_resolve_path("/sys/bus/pci");
        if (pci_dir) {
            vfs_mkdir(pci_dir, "devices", 0755);
        }
    }

    // Create /sys/subsystem/drm/devices
    vfs_mkdir(sys_root, "subsystem", 0755);
    vfs_node_t *subsys_dir = vfs_resolve_path("/sys/subsystem");
    if (subsys_dir) {
        vfs_mkdir(subsys_dir, "drm", 0755);
        vfs_node_t *subsys_drm = vfs_resolve_path("/sys/subsystem/drm");
        if (subsys_drm) {
            vfs_mkdir(subsys_drm, "devices", 0755);
            vfs_node_t *sub_dev_dir = vfs_resolve_path("/sys/subsystem/drm/devices");
            if (sub_dev_dir) {
                // Mock a link to card0
                vfs_mkdir(sub_dev_dir, "card0", 0755);
                vfs_node_t *subsys_card0 = vfs_resolve_path("/sys/subsystem/drm/devices/card0");
                if (subsys_card0) {
                     ramfs_mount_on(subsys_card0);
                     vfs_create(subsys_card0, "dev", 0444);
                     vfs_node_t *sd_dev = vfs_resolve_path("/sys/subsystem/drm/devices/card0/dev");
                     if (sd_dev) vfs_write(sd_dev, 0, 6, (uint8_t *)"226:0\n");
                }
            }
        }
        
        vfs_mkdir(subsys_dir, "input", 0755);
        vfs_node_t *subsys_input = vfs_resolve_path("/sys/subsystem/input");
        if (subsys_input) {
            vfs_mkdir(subsys_input, "devices", 0755);
        }
    }

    // Link /sys/class/drm/card0/subsystem to ../../../../subsystem/drm
    vfs_mkdir(card0_dir, "subsystem", 0755);

    klog_puts("[OK] Mock SysFS initialized at /sys\n");
}
