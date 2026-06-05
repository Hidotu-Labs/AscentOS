#ifndef DRIVERS_AUDIO_HDA_H
#define DRIVERS_AUDIO_HDA_H

#include "../../fs/vfs.h"
#include <stdbool.h>
#include <stdint.h>

// Register Offsets (HDA Spec 1.0a, Section 3.3)
#define HDA_GCAP 0x00       // Global Capabilities (2 bytes)
#define HDA_VMIN 0x02       // Minor Version (1 byte)
#define HDA_VMAJ 0x03       // Major Version (1 byte)
#define HDA_OUTPAY 0x04     // Output Payload Capability (2 bytes)
#define HDA_INPAY 0x06      // Input Payload Capability (2 bytes)
#define HDA_GCTL 0x08       // Global Control (4 bytes)
#define HDA_WAKEEN 0x0C     // Wake Enable (2 bytes)
#define HDA_STATESTS 0x0E   // State Status (2 bytes)
#define HDA_GSTS 0x10       // Global Status (2 bytes)
#define HDA_OUTSTRMPAY 0x18 // Output Stream Payload Cap (2 bytes)
#define HDA_INSTRMPAY 0x1A  // Input Stream Payload Cap (2 bytes)
#define HDA_INTCTL 0x20     // Interrupt Control (4 bytes)
#define HDA_INTSTS 0x24     // Interrupt Status (4 bytes)
#define HDA_WALCLK 0x30     // Wall Clock Counter (4 bytes)
#define HDA_SSYNC 0x38      // Stream Synchronization (4 bytes)

// CORB Registers
#define HDA_CORBLBASE 0x40 // CORB Lower Base Address (4 bytes)
#define HDA_CORBUBASE 0x44 // CORB Upper Base Address (4 bytes)
#define HDA_CORBWP 0x48    // CORB Write Pointer (2 bytes)
#define HDA_CORBRP 0x4A    // CORB Read Pointer (2 bytes)
#define HDA_CORBCTL 0x4C   // CORB Control (1 byte)
#define HDA_CORBSTS 0x4D   // CORB Status (1 byte)
#define HDA_CORBSIZE 0x4E  // CORB Size (1 byte)

// RIRB Registers
#define HDA_RIRBLBASE 0x50 // RIRB Lower Base Address (4 bytes)
#define HDA_RIRBUBASE 0x54 // RIRB Upper Base Address (4 bytes)
#define HDA_RIRBWP 0x58    // RIRB Write Pointer (2 bytes)
#define HDA_RINTCNT 0x5A   // Response Interrupt Count (2 bytes)
#define HDA_RIRBCTL 0x5C   // RIRB Control (1 byte)
#define HDA_RIRBSTS 0x5D   // RIRB Status (1 byte)
#define HDA_RIRBSIZE 0x5E  // RIRB Size (1 byte)

// Immediate Command Registers (Fallback if CORB/RIRB not used)
#define HDA_ICO 0x60 // Immediate Command Output (4 bytes)
#define HDA_IRI 0x64 // Immediate Response Input (4 bytes)
#define HDA_ICS 0x68 // Immediate Command Status (2 bytes)

// DPLBASE (DMA Position Lower Base)
#define HDA_DPLBASE 0x70
#define HDA_DPUBASE 0x74

// CORB/RIRB Constants
#define HDA_CORB_ENTRIES 256
#define HDA_RIRB_ENTRIES 256

// Parameters
#define HDA_PARAM_VENDOR_ID 0x00
#define HDA_PARAM_REVISION_ID 0x02
#define HDA_PARAM_NODE_COUNT 0x04
#define HDA_PARAM_FG_TYPE 0x05
#define HDA_PARAM_CAPS 0x09
#define HDA_PARAM_WIDGET_CAPS 0x09
#define HDA_PARAM_BEEP_CAPS 0x0A

// Widget Types
#define HDA_WIDGET_AUDIO_OUT 0x0
#define HDA_WIDGET_AUDIO_IN 0x1
#define HDA_WIDGET_AUDIO_MIX 0x2
#define HDA_WIDGET_AUDIO_SEL 0x3
#define HDA_WIDGET_PIN_COMPLEX 0x4
#define HDA_WIDGET_POWER 0x5
#define HDA_WIDGET_VOL_KNOB 0x6
#define HDA_WIDGET_BEEP_GEN 0x7
#define HDA_WIDGET_VENDOR 0xF

// Verbs
#define HDA_VERB_GET_PARAM 0xF00
#define HDA_VERB_GET_CONN_LIST 0xF02
#define HDA_VERB_SET_BEEP 0x70A
#define HDA_VERB_SET_FORMAT 0x200
#define HDA_VERB_SET_AMP_MUTE 0x300
#define HDA_VERB_SET_PIN_CTL 0x707
#define HDA_VERB_GET_PIN_CTL 0xF07
#define HDA_VERB_SET_STREAM_ID 0x706

// Stream Descriptor Registers (Start at 0x80, each is 0x20 bytes)
#define HDA_SD_BASE 0x80
#define HDA_SD_CTL 0x00   // Control (3 bytes or 4 bytes?)
#define HDA_SD_CTL 0x00   // Control (3 bytes)
#define HDA_SD_STS 0x03   // Status
#define HDA_SD_LPIB 0x04  // Link Position In Buffer
#define HDA_SD_CBL 0x08   // Cyclic Buffer Length
#define HDA_SD_LVI 0x0C   // Last Valid Index
#define HDA_SD_FIFOS 0x10 // FIFO Size
#define HDA_SD_FMT 0x12   // Format
#define HDA_SD_BDPL 0x18  // BDL Lower Address
#define HDA_SD_BDPU 0x1C  // BDL Upper Address

// SD_CTL bits
#define HDA_SD_CTL_SRST (1 << 0)
#define HDA_SD_CTL_RUN (1 << 1)
#define HDA_SD_CTL_IOCE (1 << 2) // Interrupt on Completion Enable

// BDL Entry (16 bytes)
struct hda_bdl_entry {
  uint32_t addr_low;
  uint32_t addr_high;
  uint32_t length;
  uint32_t flags; // Bit 0: IOC
} __attribute__((packed));

// Register Bits
#define HDA_GCTL_CRST (1 << 0) // Controller Reset

#define HDA_CORBCTL_RUN (1 << 1)
#define HDA_RIRBCTL_RUN (1 << 1)

// Driver Interface
void hda_init(void);
void hda_register_vfs(void);
void hda_phase1_test(void);
void hda_phase2_test(void);
void hda_phase3_test(void);
void hda_beep_test(void);
void hda_phase4_test(void);

// Send a verb and get response
uint32_t hda_send_verb(uint8_t codec, uint8_t node, uint32_t verb,
                       uint16_t payload);
uint32_t hda_send_verb_immediate(uint8_t codec, uint8_t node, uint32_t verb,
                                 uint16_t payload);

#endif
