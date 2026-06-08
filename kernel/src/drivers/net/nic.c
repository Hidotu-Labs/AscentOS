/*
 * NIC Abstraction Layer
 *
 * Probes for all supported NIC drivers and binds the first one found.
 * All network stack code should call nic_*() instead of driver-specific APIs.
 */

#include "drivers/net/nic.h"
#include "drivers/net/rtl8139.h"
#include "drivers/net/e1000.h"
#include "console/console.h"
#include "console/klog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Driver binding — which NIC is active
typedef enum {
    NIC_NONE = 0,
    NIC_RTL8139,
    NIC_E1000,
} nic_type_t;

static nic_type_t active_nic = NIC_NONE;

void nic_init(void) {
    // Try e1000 first (more capable)
    e1000_init();
    if (e1000_is_present()) {
        active_nic = NIC_E1000;
        console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Active NIC driver: Intel e1000\n");
        return;
    }

    // Fall back to RTL8139
    rtl8139_init();
    if (rtl8139_is_present()) {
        active_nic = NIC_RTL8139;
        console_puts(KLOG_CLR_GREEN "[  OK  ]" KLOG_CLR_RESET " Active NIC driver: Realtek RTL8139\n");
        return;
    }

    console_puts(KLOG_CLR_RED "[ FAIL ]" KLOG_CLR_RESET " No supported NIC found.\n");
}

const uint8_t *nic_get_mac(void) {
    switch (active_nic) {
    case NIC_E1000:   return e1000_get_mac();
    case NIC_RTL8139: return rtl8139_get_mac();
    default:          return NULL;
    }
}

bool nic_link_up(void) {
    switch (active_nic) {
    case NIC_E1000:   return e1000_link_up();
    case NIC_RTL8139: return rtl8139_link_up();
    default:          return false;
    }
}

bool nic_is_present(void) {
    return active_nic != NIC_NONE;
}

int nic_send(const void *data, uint16_t len) {
    switch (active_nic) {
    case NIC_E1000:   return e1000_send(data, len);
    case NIC_RTL8139: return rtl8139_send(data, len);
    default:          return -1;
    }
}

bool nic_poll(void) {
    switch (active_nic) {
    case NIC_E1000:   return e1000_poll();
    case NIC_RTL8139: return rtl8139_poll();
    default:          return false;
    }
}
