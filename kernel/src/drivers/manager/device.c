#include "drivers/manager/device.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "console/console.h"
#include "console/klog.h"
#include <stddef.h>

static struct device *root_node = NULL;
static struct driver *driver_list = NULL;

// Helper: dupe a string since we don't have strdup
static char *kstrdup(const char *s) {
    size_t len = strlen(s);
    char *new_s = kmalloc(len + 1);
    if (new_s) strcpy(new_s, s);
    return new_s;
}

void dm_init(void) {
    root_node = kmalloc(sizeof(struct device));
    memset(root_node, 0, sizeof(struct device));
    root_node->name = kstrdup("");
    
    // Create /sys for system devices
    device_create(root_node, "sys");
    
    console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Device Manager initialized.\n");
}

struct device *device_create(struct device *parent, const char *name) {
    struct device *dev = kmalloc(sizeof(struct device));
    if (!dev) return NULL;
    
    memset(dev, 0, sizeof(struct device));
    dev->name = kstrdup(name);
    dev->parent = parent;
    
    if (parent) {
        if (parent->first_child == NULL) {
            parent->first_child = dev;
        } else {
            struct device *curr = parent->first_child;
            while (curr->next_sibling) curr = curr->next_sibling;
            curr->next_sibling = dev;
        }
    }

    // Auto-probe the device
    dm_probe_device(dev);
    
    return dev;
}

bool device_add_resource(struct device *dev, resource_type_t type, const char *name, uint64_t start, uint64_t end) {
    if (!dev || dev->resource_count >= MAX_RESOURCES) return false;
    
    dev->resources[dev->resource_count].type = type;
    dev->resources[dev->resource_count].name = kstrdup(name);
    dev->resources[dev->resource_count].start = start;
    dev->resources[dev->resource_count].end = end;
    dev->resource_count++;
    
    return true;
}

struct device *device_find_by_path(const char *path) {
    if (!path || path[0] != '/') return NULL;
    if (path[1] == '\0') return root_node;

    struct device *curr = root_node;
    const char *p = path + 1; // Skip leading '/'
    
    char component[64];
    while (*p) {
        size_t len = 0;
        while (*p && *p != '/' && len < 63) {
            component[len++] = *p++;
        }
        component[len] = '\0';
        if (*p == '/') p++;

        // Find child with this name
        struct device *child = curr->first_child;
        bool found = false;
        while (child) {
            if (strcmp(child->name, component) == 0) {
                curr = child;
                found = true;
                break;
            }
            child = child->next_sibling;
        }
        
        if (!found) return NULL;
    }
    
    return curr;
}

static void dump_device(struct device *dev, int depth) {
    for (int i = 0; i < depth; i++) console_puts("  ");
    console_puts("- ");
    console_puts(dev->name);
    console_puts("\n");
    
    for (size_t i = 0; i < dev->resource_count; i++) {
        for (int j = 0; j < depth + 2; j++) console_puts("  ");
        console_puts("[RES] ");
        console_puts(dev->resources[i].name);
        console_puts("\n");
    }

    struct device *child = dev->first_child;
    while (child) {
        dump_device(child, depth + 1);
        child = child->next_sibling;
    }
}

static void dm_probe_all(struct device *dev) {
    if (!dev) return;
    dm_probe_device(dev);
    struct device *child = dev->first_child;
    while (child) {
        dm_probe_all(child);
        child = child->next_sibling;
    }
}

void dm_register_driver(struct driver *drv) {
    drv->next = driver_list;
    driver_list = drv;
    console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " UDM: Registered driver: ");
    console_puts(drv->name);
    console_puts("\n");

    // Trigger re-probe of all devices
    if (root_node) dm_probe_all(root_node);
}

static int match_id(struct device *dev, struct device_id *id) {
    switch (id->type) {
        case ID_ANY: return 1;
        case ID_NAME: return strcmp(dev->name, id->name) == 0;
        case ID_PCI: {
            if (id->pci.match_class) {
                return (dev->pci_class == id->pci.class &&
                        dev->pci_subclass == id->pci.subclass);
            }
            return (dev->vendor_id == id->pci.vendor && dev->device_id == id->pci.device);
        }
        case ID_ACPI: return 0; // TODO
    }
    return 0;
}

int dm_probe_device(struct device *dev) {
    if (dev->driver) return 0; // Already bound

    struct driver *drv = driver_list;
    while (drv) {
        for (size_t i = 0; i < drv->id_count; i++) {
            if (match_id(dev, &drv->ids[i])) {
                if (drv->probe(dev) == 0) {
                    dev->driver = drv;
                    console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " UDM: Bound device ");
                    console_puts(dev->name);
                    console_puts(" to driver ");
                    console_puts(drv->name);
                    console_puts("\n");
                    return 0;
                }
            }
        }
        drv = drv->next;
    }
    return -1;
}

void dm_dump_tree(void) {
    console_puts("Device Tree Hierarchy:\n");
    if (root_node) dump_device(root_node, 0);
}
