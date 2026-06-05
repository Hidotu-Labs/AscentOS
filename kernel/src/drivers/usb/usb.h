#ifndef DRIVERS_USB_USB_H
#define DRIVERS_USB_USB_H

#include <stdbool.h>
#include <stdint.h>

// USB Request Types
#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09

struct usb_control_request {
  uint8_t request_type;
  uint8_t request;
  uint16_t value;
  uint16_t index;
  uint16_t length;
} __attribute__((packed));

// Descriptor Types
#define USB_DESC_DEVICE 0x01
#define USB_DESC_CONFIGURATION 0x02
#define USB_DESC_STRING 0x03
#define USB_DESC_INTERFACE 0x04
#define USB_DESC_ENDPOINT 0x05

// Device Descriptor
struct usb_device_descriptor {
  uint8_t length;
  uint8_t type;
  uint16_t usb_version;
  uint8_t device_class;
  uint8_t device_subclass;
  uint8_t device_protocol;
  uint8_t max_packet_size;
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t device_version;
  uint8_t manufacturer_string_idx;
  uint8_t product_string_idx;
  uint8_t serial_number_idx;
  uint8_t num_configurations;
} __attribute__((packed));

// Host Controller Interface
struct usb_hcd {
  void *priv; // Pointer to controller-specific state (e.g. uhci_controller)
  int (*control_transfer)(struct usb_hcd *hcd, uint8_t addr,
                          struct usb_control_request *req, void *data,
                          uint16_t len, bool low_speed);
};

// Device Structure
struct usb_device {
  uint8_t address;
  uint8_t port;
  bool connected;
  bool low_speed;
  struct usb_device_descriptor desc;
  struct usb_hcd *hcd; // Reference to the host controller driver
};

// Core API

void usb_init(void);
void usb_enumerate_device(struct usb_device *dev);

// Called by HC driver when a new device is detected on a root hub port
void usb_device_discovered(struct usb_hcd *hcd, uint8_t port, bool low_speed);

#endif
