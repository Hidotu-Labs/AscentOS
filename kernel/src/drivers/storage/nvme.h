#ifndef STORAGE_NVME_H
#define STORAGE_NVME_H

#include <stdint.h>
#include "../manager/device.h"
#include "block.h"
#include <stdbool.h>

// NVMe Controller Registers (BAR0)
typedef struct nvme_regs {
    uint64_t cap;       // Controller Capabilities
    uint32_t vs;        // Version
    uint32_t intms;     // Interrupt Mask Set
    uint32_t intmc;     // Interrupt Mask Clear
    uint32_t cc;        // Controller Configuration
    uint32_t reserved0;
    uint32_t csts;      // Controller Status
    uint32_t nssr;      // NVM Subsystem Reset
    uint32_t aqa;       // Admin Queue Attributes
    uint64_t asq;       // Admin Submission Queue Base Address
    uint64_t acq;       // Admin Completion Queue Base Address
    uint32_t cmbloc;    // Controller Memory Buffer Location
    uint32_t cmbsz;     // Controller Memory Buffer Size
    uint32_t reserved1[12];
    // Doorbell registers start at offset 0x1000
} __attribute__((packed)) nvme_regs_t;

// NVMe Command (64 bytes)
typedef struct nvme_cmd {
    uint32_t cd0;       // Command Dword 0 (Opcode, Fuse, PSDT, CID)
    uint32_t nsid;      // Namespace Identifier
    uint64_t reserved;
    uint64_t mptr;      // Metadata Pointer
    uint64_t prp1;      // PRP Entry 1
    uint64_t prp2;      // PRP Entry 2
    uint32_t cd10;
    uint32_t cd11;
    uint32_t cd12;
    uint32_t cd13;
    uint32_t cd14;
    uint32_t cd15;
} __attribute__((packed)) nvme_cmd_t;

// NVMe Completion (16 bytes)
typedef struct nvme_completion {
    uint32_t result;    // Command specific result
    uint32_t reserved;
    uint16_t sq_head;   // SQ Head Pointer
    uint16_t sq_id;     // SQ Identifier
    uint16_t cid;       // Command Identifier
    uint16_t status;    // Status
} __attribute__((packed)) nvme_completion_t;

struct nvme_controller {
    nvme_regs_t *regs;
    uint32_t db_stride; // Doorbell stride
    

    void *admin_sq;
    void *admin_cq;
    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    
    struct device *dev;
    bool present;


    void *io_sq;
    void *io_cq;
    uint16_t io_sq_tail;
    uint16_t io_cq_head;
    uint64_t capacity_sectors;

    struct block_device bdev;


    void *msix_table_virt;
    uint8_t irq_vector;
};

void nvme_init(void);
void nvme_self_test(void);

#endif
