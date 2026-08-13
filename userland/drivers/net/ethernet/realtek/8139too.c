/*
 * Small RTL8139 userspace port for AvoryOS.
 *
 * The file deliberately follows Linux's pci_driver/net_device organization.
 * It uses the Phase 6 compatibility subset instead of Linux kernel internals;
 * it is not a verbatim copy of upstream drivers/net/ethernet/realtek/8139too.c.
 */
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/pci.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RTL_IDR0 0x00
#define RTL_TSD0 0x10
#define RTL_TSAD0 0x20
#define RTL_RBSTART 0x30
#define RTL_CR 0x37
#define RTL_CAPR 0x38
#define RTL_CBR 0x3a
#define RTL_IMR 0x3c
#define RTL_ISR 0x3e
#define RTL_TCR 0x40
#define RTL_RCR 0x44
#define RTL_CONFIG1 0x52
#define RTL_MSR 0x58

#define CR_BUFFER_EMPTY 0x01
#define CR_TX_ENABLE 0x04
#define CR_RX_ENABLE 0x08
#define CR_RESET 0x10

#define INT_RX_OK 0x0001
#define INT_RX_ERROR 0x0002
#define INT_TX_OK 0x0004
#define INT_TX_ERROR 0x0008
#define INT_RX_OVERFLOW 0x0010
#define INT_LINK_CHANGE 0x0020
#define INT_MASK (INT_RX_OK | INT_RX_ERROR | INT_TX_OK | INT_TX_ERROR | \
                  INT_RX_OVERFLOW | INT_LINK_CHANGE)

#define RCR_ACCEPT_PHYSICAL (1U << 1)
#define RCR_ACCEPT_MULTICAST (1U << 2)
#define RCR_ACCEPT_BROADCAST (1U << 3)
#define RCR_WRAP (1U << 7)
#define RCR_MAX_DMA (7U << 8)
#define RCR_NO_THRESHOLD (7U << 13)
#define TCR_MAX_DMA (7U << 8)
#define TCR_IFG96 (3U << 24)

#define RX_RING_SIZE 8192U
#define RX_DMA_SIZE (3U * 4096U)
#define TX_COUNT 4U
#define TX_BUFFER_SIZE 2048U
#define TX_DMA_SIZE (TX_COUNT * TX_BUFFER_SIZE)
#define ETH_MIN_NO_FCS 60U

struct rtl8139_private {
  struct pci_dev *pdev;
  struct net_device netdev;
  void __iomem *io;
  uint8_t *rx;
  uint8_t *tx;
  dma_addr_t rx_dma;
  dma_addr_t tx_dma;
  uint32_t rx_offset;
  uint8_t tx_next;
  uint8_t tx_busy[TX_COUNT];
  int irq_ready;
  int running;
};

static int valid_mac(const uint8_t mac[6]) {
  int zero = 1;
  int ff = 1;
  for (int i = 0; i < 6; i++) {
    zero &= mac[i] == 0;
    ff &= mac[i] == 0xff;
  }
  return !zero && !ff && !(mac[0] & 1U);
}

static void rtl8139_drain_rx(struct rtl8139_private *tp) {
  unsigned budget = 32;
  while (!(ioread8(tp->io, RTL_CR) & CR_BUFFER_EMPTY) && budget--) {
    uint32_t offset = tp->rx_offset % RX_RING_SIZE;
    uint16_t status;
    uint16_t dma_length;
    memcpy(&status, tp->rx + offset, sizeof(status));
    memcpy(&dma_length, tp->rx + offset + 2, sizeof(dma_length));
    if (!(status & 1U) || dma_length < 18 ||
        dma_length > UDRV_NET_FRAME_MAX + 4) {
      tp->rx_offset = ioread16(tp->io, RTL_CBR) % RX_RING_SIZE;
      iowrite16((uint16_t)(tp->rx_offset - 16), tp->io, RTL_CAPR);
      return;
    }
    netif_rx(&tp->netdev, tp->rx + offset + 4, dma_length - 4);
    tp->rx_offset = (offset + 4U + dma_length + 3U) & ~3U;
    tp->rx_offset %= RX_RING_SIZE;
    iowrite16((uint16_t)(tp->rx_offset - 16), tp->io, RTL_CAPR);
  }
}

static irqreturn_t rtl8139_interrupt(int irq, void *opaque) {
  (void)irq;
  struct rtl8139_private *tp = opaque;
  uint16_t status = ioread16(tp->io, RTL_ISR);
  if (!status || status == 0xffff || !(status & INT_MASK))
    return IRQ_NONE;
  iowrite16(status & INT_MASK, tp->io, RTL_ISR);
  if (status & INT_RX_OK)
    rtl8139_drain_rx(tp);
  if (status & INT_TX_OK) {
    for (unsigned i = 0; i < TX_COUNT; i++)
      if (tp->tx_busy[i]) {
        tp->tx_busy[i] = 0;
      }
  }
  if (status & (INT_RX_ERROR | INT_RX_OVERFLOW)) {
    tp->rx_offset = ioread16(tp->io, RTL_CBR) % RX_RING_SIZE;
    iowrite16((uint16_t)(tp->rx_offset - 16), tp->io, RTL_CAPR);
  }
  return IRQ_HANDLED;
}

static int rtl8139_start_xmit(struct net_device *netdev, const void *frame,
                              size_t length) {
  struct rtl8139_private *tp = netdev->priv;
  if (!tp || !tp->running || !frame || length < 14 ||
      length > UDRV_NET_FRAME_MAX - 4)
    return -1;
  unsigned slot = tp->tx_next;
  for (unsigned tries = 0; tries < TX_COUNT && tp->tx_busy[slot]; tries++)
    slot = (slot + 1U) % TX_COUNT;
  if (tp->tx_busy[slot])
    return -1;
  size_t wire_length = length < ETH_MIN_NO_FCS ? ETH_MIN_NO_FCS : length;
  uint8_t *buffer = tp->tx + slot * TX_BUFFER_SIZE;
  memcpy(buffer, frame, length);
  if (wire_length > length)
    memset(buffer + length, 0, wire_length - length);
  tp->tx_busy[slot] = 1;
  tp->tx_next = (slot + 1U) % TX_COUNT;
  iowrite32((uint32_t)wire_length, tp->io, RTL_TSD0 + slot * 4U);
  return NETDEV_TX_OK;
}

static void rtl8139_net_stop(struct net_device *netdev) {
  struct rtl8139_private *tp = netdev ? netdev->priv : NULL;
  if (!tp)
    return;
  iowrite16(0, tp->io, RTL_IMR);
  iowrite8(0, tp->io, RTL_CR);
  tp->running = 0;
  netif_carrier_off(netdev);
}

static const struct net_device_ops rtl8139_netdev_ops = {
    .ndo_start_xmit = rtl8139_start_xmit,
    .ndo_stop = rtl8139_net_stop,
};

static int rtl8139_reset(struct rtl8139_private *tp) {
  iowrite16(0, tp->io, RTL_IMR);
  iowrite16(0xffff, tp->io, RTL_ISR);
  iowrite8(0, tp->io, RTL_CONFIG1);
  iowrite8(CR_RESET, tp->io, RTL_CR);
  for (unsigned tries = 0; tries < 10000; tries++) {
    if (!(ioread8(tp->io, RTL_CR) & CR_RESET))
      return 0;
    usleep(10);
  }
  return -1;
}

static int rtl8139_probe(struct pci_dev *pdev,
                         const struct pci_device_id *id) {
  (void)id;
  struct rtl8139_private *tp = calloc(1, sizeof(*tp));
  if (!tp)
    return -1;
  tp->pdev = pdev;
  tp->io = pci_iomap(pdev, 0, 256);
  if (!tp->io || pci_enable_device(pdev) != 0 || rtl8139_reset(tp) != 0)
    return -1;

  for (int i = 0; i < 6; i++)
    tp->netdev.dev_addr[i] = ioread8(tp->io, RTL_IDR0 + (uint32_t)i);
  if (!valid_mac(tp->netdev.dev_addr))
    return -1;

  tp->rx = dma_alloc_coherent(pdev, RX_DMA_SIZE, &tp->rx_dma, GFP_KERNEL);
  tp->tx = dma_alloc_coherent(pdev, TX_DMA_SIZE, &tp->tx_dma, GFP_KERNEL);
  if (!tp->rx || !tp->tx)
    return -1;
  memset(tp->rx, 0, RX_DMA_SIZE);
  memset(tp->tx, 0, TX_DMA_SIZE);

  if (request_irq(pdev, rtl8139_interrupt, tp) != 0)
    return -1;
  tp->irq_ready = 1;
  iowrite32((uint32_t)tp->rx_dma, tp->io, RTL_RBSTART);
  for (unsigned i = 0; i < TX_COUNT; i++)
    iowrite32((uint32_t)(tp->tx_dma + i * TX_BUFFER_SIZE),
              tp->io, RTL_TSAD0 + i * 4U);
  iowrite32(TCR_IFG96 | TCR_MAX_DMA, tp->io, RTL_TCR);
  iowrite32(RCR_ACCEPT_PHYSICAL | RCR_ACCEPT_MULTICAST |
                RCR_ACCEPT_BROADCAST | RCR_WRAP | RCR_MAX_DMA |
                RCR_NO_THRESHOLD,
            tp->io, RTL_RCR);
  iowrite16((uint16_t)-16, tp->io, RTL_CAPR);
  iowrite16(0xffff, tp->io, RTL_ISR);

  snprintf(tp->netdev.name, sizeof(tp->netdev.name), "eth1");
  tp->netdev.mtu = 1500;
  tp->netdev.netdev_ops = &rtl8139_netdev_ops;
  tp->netdev.pdev = pdev;
  tp->netdev.priv = tp;
  if (register_netdev(&tp->netdev) != 0)
    return -1;

  pci_set_drvdata(pdev, tp);
  pci_set_master(pdev);
  tp->running = 1;
  iowrite8(CR_RX_ENABLE | CR_TX_ENABLE, tp->io, RTL_CR);
  iowrite16(INT_MASK, tp->io, RTL_IMR);
  if (enable_irq(pdev) != 0)
    return -1;
  if (ioread8(tp->io, RTL_MSR) & 0x04)
    netif_carrier_off(&tp->netdev);
  else
    netif_carrier_on(&tp->netdev);

  return 0;
}

static void rtl8139_remove(struct pci_dev *pdev) {
  struct rtl8139_private *tp = pci_get_drvdata(pdev);
  if (!tp)
    return;
  rtl8139_net_stop(&tp->netdev);
  free_irq(pdev);
  pci_clear_master(pdev);
  dma_free_coherent(pdev, TX_DMA_SIZE, tp->tx, tp->tx_dma);
  dma_free_coherent(pdev, RX_DMA_SIZE, tp->rx, tp->rx_dma);
  unregister_netdev(&tp->netdev);
  pci_iounmap(pdev, tp->io);
  free(tp);
  pci_set_drvdata(pdev, NULL);
}

static const struct pci_device_id rtl8139_pci_tbl[] = {
    {PCI_DEVICE(0x10ec, 0x8139)},
    {0, 0},
};

static struct pci_driver rtl8139_driver = {
    .name = "8139too-udrv",
    .id_table = rtl8139_pci_tbl,
    .probe = rtl8139_probe,
    .remove = rtl8139_remove,
};

module_pci_driver(rtl8139_driver)
