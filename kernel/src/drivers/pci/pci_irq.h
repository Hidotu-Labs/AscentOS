#ifndef PCI_IRQ_H
#define PCI_IRQ_H

#include "cpu/isr.h"
#include "drivers/pci/pci.h"

#define PCI_IRQ_MAX_VECTORS 32

enum pci_irq_mode { PCI_IRQ_NONE = 0, PCI_IRQ_MSI, PCI_IRQ_MSIX };

struct pci_irq {
  struct pci_device *dev;
  enum pci_irq_mode mode;
  uint16_t vector_count;
  uint8_t vectors[PCI_IRQ_MAX_VECTORS];
  struct pci_msi msi;
  struct pci_msix msix;
  uint16_t saved_command;
  bool command_saved;
};

/* Allocates CPU vectors, installs handlers, and routes them to dest_apic.
 * MSI-X is preferred. A one-vector request falls back to MSI. */
bool pci_irq_request(struct pci_device *dev, struct pci_irq *irq,
                     const isr_t *handlers, uint16_t count,
                     uint8_t destination_apic);
bool pci_irq_mask(struct pci_irq *irq, uint16_t index, bool masked);
void pci_irq_release(struct pci_irq *irq);

static inline uint8_t pci_irq_vector(const struct pci_irq *irq,
                                     uint16_t index) {
  return irq && index < irq->vector_count ? irq->vectors[index] : 0xFF;
}

#endif
