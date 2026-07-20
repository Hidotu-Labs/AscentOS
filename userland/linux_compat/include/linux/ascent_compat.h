#ifndef LINUX_ASCENT_COMPAT_H
#define LINUX_ASCENT_COMPAT_H

#include <ascent/udriver.h>
#include <stddef.h>
#include <stdint.h>

#define __iomem
#define GFP_KERNEL 0
#define IRQ_HANDLED 1
#define IRQ_NONE 0
#define NETDEV_TX_OK 0
#define PCI_ANY_ID 0xffffU

typedef uint64_t dma_addr_t;
typedef int irqreturn_t;

struct pci_device_id {
  uint16_t vendor;
  uint16_t device;
};

#define PCI_DEVICE(vendor_id, device_id) \
  .vendor = (vendor_id), .device = (device_id)

struct pci_dev;
struct net_device;

typedef irqreturn_t (*irq_handler_t)(int irq, void *opaque);

struct pci_driver {
  const char *name;
  const struct pci_device_id *id_table;
  int (*probe)(struct pci_dev *pdev, const struct pci_device_id *id);
  void (*remove)(struct pci_dev *pdev);
};

struct linux_dma_mapping {
  void *address;
  size_t length;
  uint32_t handle;
};

struct pci_dev {
  int control_fd;
  char name[UDRV_DEVICE_LEN];
  struct udrv_device_info info;
  struct udrv_resource_info resources[16];
  uint32_t io_resource;
  uint32_t irq_resource;
  irq_handler_t irq_handler;
  void *irq_opaque;
  struct linux_dma_mapping dma[8];
  void *driver_data;
};

struct ioport_region {
  struct pci_dev *pdev;
  uint32_t resource;
};

struct net_device_ops {
  int (*ndo_start_xmit)(struct net_device *netdev, const void *frame,
                        size_t length);
  void (*ndo_stop)(struct net_device *netdev);
};

struct net_device {
  char name[UDRV_NET_NAME_LEN];
  uint8_t dev_addr[6];
  uint16_t mtu;
  const struct net_device_ops *netdev_ops;
  struct pci_dev *pdev;
  void *priv;
  int registered;
};

int linux_pci_driver_main(struct pci_driver *driver);
int pci_enable_device(struct pci_dev *pdev);
void pci_set_master(struct pci_dev *pdev);
void pci_clear_master(struct pci_dev *pdev);
void pci_set_drvdata(struct pci_dev *pdev, void *data);
void *pci_get_drvdata(struct pci_dev *pdev);
void __iomem *pci_iomap(struct pci_dev *pdev, unsigned bar, size_t length);
void pci_iounmap(struct pci_dev *pdev, void __iomem *address);

uint8_t ioread8(const void __iomem *address, uint32_t offset);
uint16_t ioread16(const void __iomem *address, uint32_t offset);
uint32_t ioread32(const void __iomem *address, uint32_t offset);
void iowrite8(uint8_t value, void __iomem *address, uint32_t offset);
void iowrite16(uint16_t value, void __iomem *address, uint32_t offset);
void iowrite32(uint32_t value, void __iomem *address, uint32_t offset);

void *dma_alloc_coherent(struct pci_dev *pdev, size_t size,
                         dma_addr_t *dma_handle, int flags);
void dma_free_coherent(struct pci_dev *pdev, size_t size, void *address,
                       dma_addr_t dma_handle);

int request_irq(struct pci_dev *pdev, irq_handler_t handler, void *opaque);
int enable_irq(struct pci_dev *pdev);
void free_irq(struct pci_dev *pdev);

int register_netdev(struct net_device *netdev);
void unregister_netdev(struct net_device *netdev);
int netif_rx(struct net_device *netdev, const void *frame, size_t length);
void netif_carrier_on(struct net_device *netdev);
void netif_carrier_off(struct net_device *netdev);
int linux_net_state(struct net_device *netdev, struct udrv_net_state *state);

#define module_pci_driver(driver) \
  int main(void) { return linux_pci_driver_main(&(driver)); }

#endif
