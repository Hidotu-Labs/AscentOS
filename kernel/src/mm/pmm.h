#ifndef PMM_H
#define PMM_H

#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096

// Initialize basic PMM state (hhdm offset) so early components can function.
void pmm_init_early(uint64_t hhdm_offset);

// Initialize the full physical memory manager using the memory map and HHDM
// offset.
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

// New Buddy Allocator API
void *pmm_alloc_page(void); // Allocate single page
void *pmm_alloc_pages(
    size_t count); // Allocate multiple (will allocate ceil(log2(count)))
void *pmm_alloc_pages_constrained(size_t count, uint64_t max_phys_addr);
void *pmm_alloc_pages_range(size_t count, uint64_t min_phys_addr,
                            uint64_t max_phys_addr);
void pmm_free_page(void *ptr);                // Free single page
void pmm_free_pages(void *ptr, size_t count); // Free multiple pages

// Refcounting (for CoW)
void pmm_incref(void *ptr);      // Increment reference count
void pmm_decref(void *ptr);      // Decrement reference count (frees if 0)
uint16_t pmm_get_ref(void *ptr); // Get current reference count
bool pmm_is_managed(
    uint64_t phys); // Check if page is managed by PMM (RAM vs MMIO)

// Compatibility aliases for existing code
#define pmm_alloc pmm_alloc_page
#define pmm_alloc_blocks pmm_alloc_pages
#define pmm_free pmm_free_page
#define pmm_free_blocks pmm_free_pages

void pmm_mark_used(void *ptr, size_t count);

// Reclaims the memory occupied by the Limine bootloader after boot structures
// are no longer needed
void pmm_reclaim_bootloader(uint64_t kernel_phys_base);
bool pmm_is_reclaimable(uint64_t phys);

// Return usable memory in bytes
uint64_t pmm_get_usable_memory(void);

// Return total generic memory in bytes
uint64_t pmm_get_total_memory(void);

// Expose the HHDM base
uint64_t pmm_get_hhdm_offset(void);

// Statistics
size_t pmm_get_free_pages(void);

#endif
