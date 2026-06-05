#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    RES_NONE,
    RES_MEM,    // MMIO range
    RES_IO,     // Port I/O range
    RES_IRQ     // Interrupt number
} resource_type_t;

typedef enum {
    ID_ANY,
    ID_PCI,
    ID_ACPI,
    ID_NAME
} device_id_type_t;

struct device_id {
    device_id_type_t type;
    union {
        struct {
            uint16_t vendor;
            uint16_t device;
            uint8_t class;
            uint8_t subclass;
            uint8_t prog_if;
            bool match_class;
        } pci;
        const char *acpi_hid;
        const char *name;
    };
};

struct resource {
    resource_type_t type;
    const char *name;
    uint64_t start;
    uint64_t end;
    uint64_t flags;
};

#define MAX_RESOURCES 16

struct device {
    const char *name;
    struct device *parent;
    
    struct device *first_child;
    struct device *next_sibling;

    struct resource resources[MAX_RESOURCES];
    size_t resource_count;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t pci_class;
    uint8_t pci_subclass;
    uint8_t pci_prog_if;

    struct driver *driver;
    void *driver_data; // Pointer to driver-specific state
};

struct driver {
    const char *name;
    struct device_id *ids;
    size_t id_count;

    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);

    struct driver *next;
};

// Initialize the device manager and the root node
void dm_init(void);

// Create a new device node as a child of another
struct device *device_create(struct device *parent, const char *name);

// Add a resource to a device
bool device_add_resource(struct device *dev, resource_type_t type, const char *name, uint64_t start, uint64_t end);

// Find a device by its absolute path (e.g. "/sys/pci/00:02.0")
struct device *device_find_by_path(const char *path);

// Log the device tree to the console (for debugging)
void dm_dump_tree(void);


void dm_register_driver(struct driver *drv);
int dm_probe_device(struct device *dev);

#endif
