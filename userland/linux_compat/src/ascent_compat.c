#include <linux/ascent_compat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static struct pci_dev active_pdev;
static struct pci_driver *active_driver;
static struct net_device *active_netdev;

static int read_hex_file(const char *path, unsigned *value) {
  FILE *file = fopen(path, "r");
  if (!file)
    return -1;
  int result = fscanf(file, "%x", value) == 1 ? 0 : -1;
  fclose(file);
  return result;
}

static const struct pci_device_id *match_id(const struct pci_device_id *ids,
                                             unsigned vendor,
                                             unsigned device) {
  if (!ids)
    return NULL;
  for (; ids->vendor || ids->device; ids++)
    if ((ids->vendor == PCI_ANY_ID || ids->vendor == vendor) &&
        (ids->device == PCI_ANY_ID || ids->device == device))
      return ids;
  return NULL;
}

static int register_control_driver(int fd, struct pci_driver *driver) {
  struct udrv_register request;
  memset(&request, 0, sizeof(request));
  request.abi_version = UDRV_ABI_VERSION;
  request.id_count = 1;
  snprintf(request.driver_name, sizeof(request.driver_name), "%s",
           driver->name);
  snprintf(request.bus_name, sizeof(request.bus_name), "pci");
  request.ids[0].flags = UDRV_MATCH_PCI_ID;
  request.ids[0].vendor = driver->id_table[0].vendor;
  request.ids[0].device = driver->id_table[0].device;
  return ioctl(fd, UDRV_IOCTL_REGISTER, &request);
}

static int claim_first_match(int fd, struct pci_driver *driver,
                             struct pci_dev *pdev,
                             const struct pci_device_id **matched) {
  DIR *dir = opendir("/sys/bus/pci/devices");
  if (!dir)
    return -1;
  struct dirent *entry;
  int result = -1;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "0000:", 5) != 0)
      continue;
    char path[256];
    unsigned vendor = 0, device = 0;
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.31s/vendor",
             entry->d_name);
    if (read_hex_file(path, &vendor) != 0)
      continue;
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.31s/device",
             entry->d_name);
    if (read_hex_file(path, &device) != 0)
      continue;
    const struct pci_device_id *id =
        match_id(driver->id_table, vendor, device);
    if (!id)
      continue;
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%.31s/driver",
             entry->d_name);
    struct stat st;
    if (lstat(path, &st) == 0)
      continue;

    struct udrv_claim claim;
    memset(&claim, 0, sizeof(claim));
    snprintf(claim.device_name, sizeof(claim.device_name), "%.31s",
             entry->d_name);
    if (ioctl(fd, UDRV_IOCTL_CLAIM, &claim) != 0)
      continue;
    memset(pdev, 0, sizeof(*pdev));
    pdev->control_fd = fd;
    pdev->io_resource = UINT32_MAX;
    pdev->irq_resource = UINT32_MAX;
    snprintf(pdev->name, sizeof(pdev->name), "%.31s", entry->d_name);
    snprintf(pdev->info.device_name, sizeof(pdev->info.device_name), "%s",
             pdev->name);
    if (ioctl(fd, UDRV_IOCTL_GET_DEVICE, &pdev->info) != 0)
      break;
    for (uint32_t i = 0;
         i < pdev->info.resource_count && i < 16; i++) {
      pdev->resources[i].index = i;
      snprintf(pdev->resources[i].device_name,
               sizeof(pdev->resources[i].device_name), "%s", pdev->name);
      if (ioctl(fd, UDRV_IOCTL_GET_RESOURCE, &pdev->resources[i]) != 0)
        continue;
      if (pdev->resources[i].type == UDRV_RESOURCE_IO &&
          pdev->io_resource == UINT32_MAX)
        pdev->io_resource = i;
      if (pdev->resources[i].type == UDRV_RESOURCE_IRQ &&
          pdev->irq_resource == UINT32_MAX)
        pdev->irq_resource = i;
    }
    *matched = id;
    result = 0;
    break;
  }
  closedir(dir);
  return result;
}

int pci_enable_device(struct pci_dev *pdev) {
  struct udrv_pci_config config;
  memset(&config, 0, sizeof(config));
  snprintf(config.device_name, sizeof(config.device_name), "%s", pdev->name);
  config.offset = 4;
  config.width = 2;
  config.write = 1;
  config.value = 3;
  return ioctl(pdev->control_fd, UDRV_IOCTL_PCI_CONFIG, &config);
}

void pci_set_master(struct pci_dev *pdev) {
  struct udrv_bus_master request;
  memset(&request, 0, sizeof(request));
  snprintf(request.device_name, sizeof(request.device_name), "%s", pdev->name);
  request.enable = 1;
  ioctl(pdev->control_fd, UDRV_IOCTL_BUS_MASTER, &request);
}

void pci_clear_master(struct pci_dev *pdev) {
  struct udrv_bus_master request;
  memset(&request, 0, sizeof(request));
  snprintf(request.device_name, sizeof(request.device_name), "%s", pdev->name);
  request.enable = 0;
  ioctl(pdev->control_fd, UDRV_IOCTL_BUS_MASTER, &request);
}

void pci_set_drvdata(struct pci_dev *pdev, void *data) {
  pdev->driver_data = data;
}

void *pci_get_drvdata(struct pci_dev *pdev) {
  return pdev->driver_data;
}

void __iomem *pci_iomap(struct pci_dev *pdev, unsigned bar, size_t length) {
  (void)bar;
  if (!pdev || pdev->io_resource == UINT32_MAX ||
      (length && length > pdev->resources[pdev->io_resource].length))
    return NULL;
  struct ioport_region *region = calloc(1, sizeof(*region));
  if (!region)
    return NULL;
  region->pdev = pdev;
  region->resource = pdev->io_resource;
  return region;
}

void pci_iounmap(struct pci_dev *pdev, void __iomem *address) {
  (void)pdev;
  free(address);
}

static uint32_t port_read(const void __iomem *address, uint32_t offset,
                          uint8_t width) {
  const struct ioport_region *region = address;
  struct udrv_port_io request;
  memset(&request, 0, sizeof(request));
  snprintf(request.device_name, sizeof(request.device_name), "%s",
           region->pdev->name);
  request.resource_index = region->resource;
  request.offset = offset;
  request.width = width;
  if (ioctl(region->pdev->control_fd, UDRV_IOCTL_PORT_IO, &request) != 0)
    return ~0U;
  return request.value;
}

static void port_write(void __iomem *address, uint32_t offset,
                       uint8_t width, uint32_t value) {
  struct ioport_region *region = address;
  struct udrv_port_io request;
  memset(&request, 0, sizeof(request));
  snprintf(request.device_name, sizeof(request.device_name), "%s",
           region->pdev->name);
  request.resource_index = region->resource;
  request.offset = offset;
  request.width = width;
  request.write = 1;
  request.value = value;
  ioctl(region->pdev->control_fd, UDRV_IOCTL_PORT_IO, &request);
}

uint8_t ioread8(const void __iomem *address, uint32_t offset) {
  return (uint8_t)port_read(address, offset, 1);
}
uint16_t ioread16(const void __iomem *address, uint32_t offset) {
  return (uint16_t)port_read(address, offset, 2);
}
uint32_t ioread32(const void __iomem *address, uint32_t offset) {
  return port_read(address, offset, 4);
}
void iowrite8(uint8_t value, void __iomem *address, uint32_t offset) {
  port_write(address, offset, 1, value);
}
void iowrite16(uint16_t value, void __iomem *address, uint32_t offset) {
  port_write(address, offset, 2, value);
}
void iowrite32(uint32_t value, void __iomem *address, uint32_t offset) {
  port_write(address, offset, 4, value);
}

void *dma_alloc_coherent(struct pci_dev *pdev, size_t size,
                         dma_addr_t *dma_handle, int flags) {
  (void)flags;
  struct udrv_dma_alloc request;
  memset(&request, 0, sizeof(request));
  snprintf(request.device_name, sizeof(request.device_name), "%s", pdev->name);
  request.size = size;
  request.flags = UDRV_DMA_F_32BIT | UDRV_DMA_F_TRUSTED;
  if (ioctl(pdev->control_fd, UDRV_IOCTL_DMA_ALLOC, &request) != 0)
    return NULL;
  void *address = mmap(NULL, request.length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, pdev->control_fd, request.mmap_offset);
  if (address == MAP_FAILED) {
    struct udrv_dma_free release;
    memset(&release, 0, sizeof(release));
    snprintf(release.device_name, sizeof(release.device_name), "%s",
             pdev->name);
    release.handle = request.handle;
    ioctl(pdev->control_fd, UDRV_IOCTL_DMA_FREE, &release);
    return NULL;
  }
  for (unsigned i = 0; i < 8; i++)
    if (!pdev->dma[i].address) {
      pdev->dma[i].address = address;
      pdev->dma[i].length = request.length;
      pdev->dma[i].handle = request.handle;
      break;
    }
  if (dma_handle)
    *dma_handle = request.dma_address;
  return address;
}

void dma_free_coherent(struct pci_dev *pdev, size_t size, void *address,
                       dma_addr_t dma_handle) {
  (void)size;
  (void)dma_handle;
  for (unsigned i = 0; i < 8; i++) {
    if (pdev->dma[i].address != address)
      continue;
    munmap(address, pdev->dma[i].length);
    struct udrv_dma_free release;
    memset(&release, 0, sizeof(release));
    snprintf(release.device_name, sizeof(release.device_name), "%s",
             pdev->name);
    release.handle = pdev->dma[i].handle;
    ioctl(pdev->control_fd, UDRV_IOCTL_DMA_FREE, &release);
    memset(&pdev->dma[i], 0, sizeof(pdev->dma[i]));
    return;
  }
}

int request_irq(struct pci_dev *pdev, irq_handler_t handler, void *opaque) {
  if (!pdev || pdev->irq_resource == UINT32_MAX || !handler)
    return -1;
  struct udrv_irq_setup request;
  memset(&request, 0, sizeof(request));
  snprintf(request.device_name, sizeof(request.device_name), "%s", pdev->name);
  request.resource_index = pdev->irq_resource;
  request.vector_count = 1;
  request.flags = UDRV_IRQ_F_MASK_ON_EVENT;
  if (ioctl(pdev->control_fd, UDRV_IOCTL_IRQ_SETUP, &request) != 0)
    return -1;
  pdev->irq_handler = handler;
  pdev->irq_opaque = opaque;
  return 0;
}

int enable_irq(struct pci_dev *pdev) {
  struct udrv_irq_control request;
  memset(&request, 0, sizeof(request));
  snprintf(request.device_name, sizeof(request.device_name), "%s", pdev->name);
  request.action = UDRV_IRQ_ACTION_UNMASK;
  return ioctl(pdev->control_fd, UDRV_IOCTL_IRQ_CONTROL, &request);
}

void free_irq(struct pci_dev *pdev) {
  struct udrv_irq_control request;
  memset(&request, 0, sizeof(request));
  snprintf(request.device_name, sizeof(request.device_name), "%s", pdev->name);
  request.action = UDRV_IRQ_ACTION_RELEASE;
  ioctl(pdev->control_fd, UDRV_IOCTL_IRQ_CONTROL, &request);
  pdev->irq_handler = NULL;
}

int register_netdev(struct net_device *netdev) {
  struct udrv_net_register request;
  memset(&request, 0, sizeof(request));
  snprintf(request.device_name, sizeof(request.device_name), "%s",
           netdev->pdev->name);
  snprintf(request.interface_name, sizeof(request.interface_name), "%s",
           netdev->name);
  memcpy(request.mac, netdev->dev_addr, sizeof(request.mac));
  request.mtu = netdev->mtu;
  request.flags = UDRV_NET_F_LINK_UP;
  int result =
      ioctl(netdev->pdev->control_fd, UDRV_IOCTL_NET_REGISTER, &request);
  if (result == 0) {
    netdev->registered = 1;
    active_netdev = netdev;
  }
  return result;
}

void unregister_netdev(struct net_device *netdev) {
  if (netdev)
    netdev->registered = 0;
  if (active_netdev == netdev)
    active_netdev = NULL;
}

int netif_rx(struct net_device *netdev, const void *frame, size_t length) {
  if (!netdev || !netdev->registered || length > UDRV_NET_FRAME_MAX)
    return -1;
  struct udrv_net_frame request;
  memset(&request, 0, sizeof(request));
  snprintf(request.device_name, sizeof(request.device_name), "%s",
           netdev->pdev->name);
  request.length = (uint16_t)length;
  memcpy(request.data, frame, length);
  return ioctl(netdev->pdev->control_fd, UDRV_IOCTL_NET_RX, &request);
}

static void set_carrier(struct net_device *netdev, int up) {
  struct udrv_net_state state;
  memset(&state, 0, sizeof(state));
  snprintf(state.device_name, sizeof(state.device_name), "%s",
           netdev->pdev->name);
  state.flags = up ? UDRV_NET_F_LINK_UP : 0;
  ioctl(netdev->pdev->control_fd, UDRV_IOCTL_NET_STATE, &state);
}

void netif_carrier_on(struct net_device *netdev) { set_carrier(netdev, 1); }
void netif_carrier_off(struct net_device *netdev) { set_carrier(netdev, 0); }

int linux_net_state(struct net_device *netdev, struct udrv_net_state *state) {
  memset(state, 0, sizeof(*state));
  snprintf(state->device_name, sizeof(state->device_name), "%s",
           netdev->pdev->name);
  state->flags = UDRV_NET_F_LINK_UP;
  return ioctl(netdev->pdev->control_fd, UDRV_IOCTL_NET_STATE, state);
}

static void drain_kernel_tx(void) {
  if (!active_netdev || !active_netdev->netdev_ops ||
      !active_netdev->netdev_ops->ndo_start_xmit)
    return;
  for (;;) {
    struct udrv_net_frame frame;
    memset(&frame, 0, sizeof(frame));
    snprintf(frame.device_name, sizeof(frame.device_name), "%s",
             active_netdev->pdev->name);
    if (ioctl(active_netdev->pdev->control_fd, UDRV_IOCTL_NET_TX, &frame) != 0)
      break;
    active_netdev->netdev_ops->ndo_start_xmit(
        active_netdev, frame.data, frame.length);
  }
}

static void dispatch_irq(struct pci_dev *pdev) {
  struct udrv_irq_event event;
  ssize_t length = read(pdev->control_fd, &event, sizeof(event));
  if (length != (ssize_t)sizeof(event))
    return;
  if (pdev->irq_handler)
    pdev->irq_handler((int)event.vector, pdev->irq_opaque);
  struct udrv_irq_control ack;
  memset(&ack, 0, sizeof(ack));
  snprintf(ack.device_name, sizeof(ack.device_name), "%s", pdev->name);
  ack.action = UDRV_IRQ_ACTION_ACK;
  ioctl(pdev->control_fd, UDRV_IOCTL_IRQ_CONTROL, &ack);
}

int linux_pci_driver_main(struct pci_driver *driver) {
  int fd = open("/dev/driverctl", O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    perror("open /dev/driverctl");
    return 1;
  }
  if (!driver || !driver->name || !driver->id_table ||
      register_control_driver(fd, driver) != 0) {
    perror("register PCI driver");
    close(fd);
    return 1;
  }
  const struct pci_device_id *matched = NULL;
  if (claim_first_match(fd, driver, &active_pdev, &matched) != 0) {
    printf("[UDRIVER NET] no unbound %s device; service skipped\n",
           driver->name);
    close(fd);
    return 0;
  }
  active_driver = driver;
  if (driver->probe(&active_pdev, matched) != 0) {
    fprintf(stderr, "[UDRIVER NET] %s probe failed on %s\n",
            driver->name, active_pdev.name);
    close(fd);
    return 1;
  }

  for (;;) {
    struct pollfd wait = {.fd = fd, .events = POLLIN};
    if (poll(&wait, 1, 100) < 0 && errno != EINTR)
      break;
    drain_kernel_tx();
    dispatch_irq(&active_pdev);
  }
  if (active_driver && active_driver->remove)
    active_driver->remove(&active_pdev);
  close(fd);
  return 0;
}
