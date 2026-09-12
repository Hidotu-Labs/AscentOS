/* AvoryOS LinuxKPI kernel configuration.
 *
 * This file is checked in and maintained by hand.  AvoryOS does not run
 * Kconfig: imported Linux code sees only the symbols defined here, and a
 * symbol that is absent is "disabled" (Linux's IS_ENABLED() treats an
 * undefined CONFIG as 0, so disabled symbols must NOT be defined as 0).
 *
 * Keep the set minimal.  Every symbol should be justified by an imported
 * file that reads it.  Phase 0 only needs enough for leaf library files;
 * later phases grow this file (DRM, TTM, amdgpu, ...).
 *
 * Deliberately absent:
 *   CONFIG_MODULES          no module loader; everything is linked in
 *   CONFIG_PM*              no runtime/system power management yet
 *   CONFIG_ACPI             native ACPI only has MADT/FADT/MCFG/HPET
 *   CONFIG_IOMMU_SUPPORT    DMA is identity-mapped on x86 for now
 *   CONFIG_DEBUG_FS         no debugfs yet
 *   CONFIG_TRACEPOINTS      tracepoints compile to no-ops
 *   CONFIG_JUMP_LABEL       static branches fall back to atomics
 *   CONFIG_HAVE_STATIC_CALL static calls fall back to indirect calls
 *   CONFIG_KASAN/KCSAN/KMSAN, CONFIG_MEMCG, CONFIG_SLUB/SLAB
 */
#ifndef __AVORY_LINUXKPI_AUTOCONF_H
#define __AVORY_LINUXKPI_AUTOCONF_H

/* Architecture. */
#define CONFIG_X86 1
#define CONFIG_X86_64 1
#define CONFIG_64BIT 1
#define CONFIG_MMU 1
#define CONFIG_SMP 1

/* Architected page-table geometry: 4-level x86_64 (matches the native VMM). */
#define CONFIG_PGTABLE_LEVELS 4

/* x86 cache geometry: 64-byte lines on every x86_64 machine; the internode
 * shift matches what Linux uses for non-VSMP x86_64. */
#define CONFIG_X86_L1_CACHE_SHIFT 6
#define CONFIG_X86_INTERNODE_CACHE_SHIFT 6

/* Core kernel. */
#define CONFIG_BUG 1
#define CONFIG_BASE_SMALL 0
#define CONFIG_GENERIC_BUG 1
#define CONFIG_GENERIC_BUG_RELATIVE_POINTERS 1
#define CONFIG_PRINTK 1
#define CONFIG_HZ 1000
#define CONFIG_HZ_1000 1

/* Debugging poison value for bad kernel pointers (x86_64 default). */
#define CONFIG_ILLEGAL_POINTER_VALUE 0xdead000000000000

/* Buses and drivers. */
#define CONFIG_PCI 1
#define CONFIG_PCI_MSI 1

/* Graphics (Phase 3+). */
#define CONFIG_DRM 1
#define CONFIG_DRM_KMS_HELPER 1
#define CONFIG_DMA_SHARED_BUFFER 1

/* Allocators used by imported library code. */
#define CONFIG_GENERIC_ALLOCATOR 1

#endif /* __AVORY_LINUXKPI_AUTOCONF_H */
