#include "usb.h"
#include "../../console/klog.h"
#include "../../io/io.h"
#include "../../mm/heap.h"
#include "uhci.h"
#include "usb_kbd.h"
#include "usb_mouse.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_USB_DEVICES 128
static struct usb_device *devices[MAX_USB_DEVICES];
static int device_count = 0;

void usb_enumerate_device(struct usb_device *dev);

static bool usb_initialized = false;

void usb_init(void) {
  if (usb_initialized)
    return;
  usb_initialized = true;
  klog_puts("[USB] Subsystem initialized.\n");
  for (int i = 0; i < MAX_USB_DEVICES; i++)
    devices[i] = NULL;
}

void usb_device_discovered(struct usb_hcd *hcd, uint8_t port, bool low_speed) {
  klog_puts("[USB] New device detected on port ");
  klog_uint64(port + 1);
  klog_puts(low_speed ? " (Low-Speed)\n" : " (Full-Speed)\n");

  if (device_count >= MAX_USB_DEVICES)
    return;

  struct usb_device *dev = kmalloc(sizeof(struct usb_device));
  if (!dev)
    return;

  dev->address = 0; // Not yet assigned
  dev->port = port;
  dev->connected = true;
  dev->low_speed = low_speed;
  dev->hcd = hcd;

  devices[device_count++] = dev;


  usb_enumerate_device(dev);
}

void usb_enumerate_device(struct usb_device *dev) {
  klog_puts("[USB] Enumerating device...\n");

  struct usb_control_request req;

  // 1. Get first 8 bytes of Device Descriptor to get max packet size
  req.request_type = 0x80; // IN
  req.request = USB_REQ_GET_DESCRIPTOR;
  req.value = (USB_DESC_DEVICE << 8);
  req.index = 0;
  req.length = 8;

  int res = dev->hcd->control_transfer(dev->hcd, 0, &req, &dev->desc, 8,
                                       dev->low_speed);
  if (res < 0) {
    klog_puts("[USB] Failed to get device descriptor (8 bytes)\n");
    return;
  }

  klog_puts("[USB] Max Packet Size: ");
  klog_uint64(dev->desc.max_packet_size);
  klog_puts("\n");

  // 2. Set Address
  uint8_t new_addr = device_count; // Simple addressing for now
  req.request_type = 0x00;         // OUT
  req.request = USB_REQ_SET_ADDRESS;
  req.value = new_addr;
  req.index = 0;
  req.length = 0;

  res = dev->hcd->control_transfer(dev->hcd, 0, &req, NULL, 0, dev->low_speed);
  if (res < 0) {
    klog_puts("[USB] Failed to set address\n");
    return;
  }

  dev->address = new_addr;
  for (int i = 0; i < 1000; i++)
    io_wait(); // Wait for address to settle

  // 3. Get Full Device Descriptor
  req.request_type = 0x80; // IN
  req.request = USB_REQ_GET_DESCRIPTOR;
  req.value = (USB_DESC_DEVICE << 8);
  req.index = 0;
  req.length = 18;

  res = dev->hcd->control_transfer(dev->hcd, dev->address, &req, &dev->desc, 18,
                                   dev->low_speed);
  if (res < 0) {
    klog_puts("[USB] Failed to get full device descriptor\n");
    return;
  }

  klog_puts("[USB] Device Vendor: ");
  klog_hex32(dev->desc.vendor_id);
  klog_puts(" Product: ");
  klog_hex32(dev->desc.product_id);
  klog_puts("\n");


  if (usb_kbd_probe(dev)) {
    klog_puts("[USB] USB Keyboard driver attached\n");
  } else if (usb_mouse_probe(dev)) {
    klog_puts("[USB] USB Mouse driver attached\n");
  }
}
