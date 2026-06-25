#ifndef FS_SYSFS_H
#define FS_SYSFS_H

void sysfs_init(void);

// GPU device path for netlink uevents (e.g. "/devices/pci0000:00/0000:00:02.0/drm/card0")
extern char sysfs_gpu_devpath[128];
extern char sysfs_gpu_connector_devpath[128];

#endif
