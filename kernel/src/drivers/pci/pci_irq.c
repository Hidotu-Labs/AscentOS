#include "drivers/pci/pci_irq.h"
#include "lib/string.h"

#define PCI_COMMAND_INTX_DISABLE (1U << 10)

static void disable_intx(struct pci_irq *irq) {
  struct pci_device *d = irq->dev;
  irq->saved_command = pci_config_read16(d->bus, d->slot, d->func, 0x04);
  irq->command_saved = true;
  pci_config_write16(d->bus, d->slot, d->func, 0x04,
                     irq->saved_command | PCI_COMMAND_INTX_DISABLE);
}

static void restore_intx(struct pci_irq *irq) {
  if (!irq->command_saved || !irq->dev) return;
  struct pci_device *d = irq->dev;
  uint16_t command = pci_config_read16(d->bus, d->slot, d->func, 0x04);
  command = (command & ~PCI_COMMAND_INTX_DISABLE) |
            (irq->saved_command & PCI_COMMAND_INTX_DISABLE);
  pci_config_write16(d->bus, d->slot, d->func, 0x04, command);
  irq->command_saved = false;
}

static void free_vectors(struct pci_irq *irq) {
  for (uint16_t i = 0; i < irq->vector_count; i++) {
    if (irq->vectors[i] != 0xFF) {
      interrupt_vector_free(irq->vectors[i]);
      irq->vectors[i] = 0xFF;
    }
  }
  irq->vector_count = 0;
}

static bool alloc_vectors(struct pci_irq *irq, const isr_t *handlers,
                          uint16_t count) {
  for (uint16_t i = 0; i < count; i++) {
    int vector = interrupt_vector_alloc(handlers[i]);
    if (vector < 0) return false;
    irq->vectors[i] = (uint8_t)vector;
    irq->vector_count++;
  }
  return true;
}

static bool try_msix(struct pci_irq *irq, const isr_t *handlers,
                     uint16_t count, uint8_t destination_apic) {
  if (!pci_msix_init(irq->dev, &irq->msix) || irq->msix.table_size < count)
    return false;
  if (!alloc_vectors(irq, handlers, count)) goto fail;
  for (uint16_t i = 0; i < count; i++)
    if (!pci_msix_program(&irq->msix, i, irq->vectors[i], destination_apic))
      goto fail;
  if (!pci_msix_enable(&irq->msix)) goto fail;
  for (uint16_t i = 0; i < count; i++) pci_msix_mask(&irq->msix, i, false);
  irq->mode = PCI_IRQ_MSIX;
  return true;
fail:
  pci_msix_disable(&irq->msix);
  free_vectors(irq);
  return false;
}

bool pci_irq_request(struct pci_device *dev, struct pci_irq *irq,
                     const isr_t *handlers, uint16_t count,
                     uint8_t destination_apic) {
  if (!dev || !irq || !handlers || !count || count > PCI_IRQ_MAX_VECTORS)
    return false;
  for (uint16_t i = 0; i < count; i++) if (!handlers[i]) return false;
  memset(irq, 0, sizeof(*irq));
  irq->dev = dev;
  for (uint16_t i = 0; i < PCI_IRQ_MAX_VECTORS; i++) irq->vectors[i] = 0xFF;
  if (try_msix(irq, handlers, count, destination_apic)) {
    disable_intx(irq);
    return true;
  }
  pci_msix_disable(&irq->msix);
  memset(&irq->msix, 0, sizeof(irq->msix));
  if (count == 1 && alloc_vectors(irq, handlers, 1) &&
      pci_msi_enable(dev, &irq->msi, irq->vectors[0], destination_apic)) {
    irq->mode = PCI_IRQ_MSI;
    disable_intx(irq);
    return true;
  }
  pci_irq_release(irq);
  return false;
}

bool pci_irq_mask(struct pci_irq *irq, uint16_t index, bool masked) {
  if (!irq || index >= irq->vector_count) return false;
  if (irq->mode == PCI_IRQ_MSIX) {
    pci_msix_mask(&irq->msix, index, masked);
    return true;
  }
  if (irq->mode == PCI_IRQ_MSI && index == 0) {
    struct pci_device *d = irq->dev;
    uint16_t control = pci_config_read16(d->bus, d->slot, d->func,
                                         irq->msi.capability + 2);
    if (masked) control &= ~1U; else control |= 1U;
    pci_config_write16(d->bus, d->slot, d->func,
                       irq->msi.capability + 2, control);
    irq->msi.enabled = !masked;
    return true;
  }
  return false;
}

void pci_irq_release(struct pci_irq *irq) {
  if (!irq) return;
  if (irq->mode == PCI_IRQ_MSIX || irq->msix.initialized) {
    for (uint16_t i = 0; i < irq->vector_count; i++)
      pci_msix_mask(&irq->msix, i, true);
    pci_msix_disable(&irq->msix);
  } else if (irq->mode == PCI_IRQ_MSI || irq->msi.dev) {
    pci_msi_disable(&irq->msi);
  }
  restore_intx(irq);
  free_vectors(irq);
  irq->mode = PCI_IRQ_NONE;
  irq->dev = NULL;
}
