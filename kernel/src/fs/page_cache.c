#include "vfs.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../console/klog.h"
#include "../lib/string.h"

#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

#define VFS_CACHE_HASH(offset) (((offset) >> 12) & 0x1F)

vfs_page_t *vfs_cache_lookup(vfs_node_t *node, uint32_t offset) {
    if (!node) return NULL;
    
    uint32_t bucket = VFS_CACHE_HASH(offset);
    spinlock_acquire(&node->pages_lock);
    vfs_page_t *page;
    list_for_each_entry(page, &node->pages[bucket], list) {
        if (page->offset == offset) {
            list_del(&page->list);
            list_add(&page->list, &node->pages[bucket]);
            spinlock_release(&node->pages_lock);
            return page;
        }
    }
    spinlock_release(&node->pages_lock);
    return NULL;
}

vfs_page_t *vfs_cache_insert(vfs_node_t *node, uint32_t offset, uint64_t frame) {
    if (!node) return NULL;
    
    vfs_page_t *new_page = kmalloc(sizeof(vfs_page_t));
    if (!new_page) return NULL;
    
    new_page->offset = offset;
    new_page->frame_phys = frame;
    new_page->dirty = false;
    
    uint32_t bucket = VFS_CACHE_HASH(offset);
    spinlock_acquire(&node->pages_lock);
    
    // Safety check: make sure it wasn't inserted while we were allocating
    vfs_page_t *page;
    list_for_each_entry(page, &node->pages[bucket], list) {
        if (page->offset == offset) {
            spinlock_release(&node->pages_lock);
            kfree(new_page);
            return page;
        }
    }
    
    list_add_tail(&new_page->list, &node->pages[bucket]);
    spinlock_release(&node->pages_lock);
    
    return new_page;
}

vfs_page_t *vfs_cache_get_or_create(vfs_node_t *node, uint32_t offset) {
    vfs_page_t *page = vfs_cache_lookup(node, offset);
    if (page) return page;

    void *frame = pmm_alloc_page();
    if (!frame) return NULL;

    page = vfs_cache_insert(node, offset, (uint64_t)frame);
    if (!page) {
        pmm_free_page(frame);
    }
    return page;
}

void vfs_cache_invalidate(vfs_node_t *node, uint32_t offset) {
    if (!node) return;
    
    uint32_t bucket = VFS_CACHE_HASH(offset);
    spinlock_acquire(&node->pages_lock);
    vfs_page_t *page, *n;
    list_for_each_entry_safe(page, n, &node->pages[bucket], list) {
        if (page->offset == offset) {
            list_del(&page->list);
            pmm_free_page((void*)page->frame_phys);
            kfree(page);
            break;
        }
    }
    spinlock_release(&node->pages_lock);
}

void vfs_cache_invalidate_range(vfs_node_t *node, uint32_t offset, uint32_t length) {
    if (!node || length == 0) return;
    
    uint32_t start_page = offset >> 12;
    uint32_t end_page = (offset + length + 4095) >> 12;
    
    spinlock_acquire(&node->pages_lock);

    // Optimization: If the range covers more than half the file or is very large, 
    // just clear the whole cache. This is faster than walking and checking every page.
    if (length >= node->length / 2 || length > 1024 * 1024 * 32) {
        for (int i = 0; i < 32; i++) {
            vfs_page_t *page, *n;
            list_for_each_entry_safe(page, n, &node->pages[i], list) {
                list_del(&page->list);
                pmm_free_page((void*)page->frame_phys);
                kfree(page);
            }
        }
        spinlock_release(&node->pages_lock);
        return;
    }
    
    for (int i = 0; i < 32; i++) {
        vfs_page_t *page, *n;
        list_for_each_entry_safe(page, n, &node->pages[i], list) {
            uint32_t p_idx = page->offset >> 12;
            if (p_idx >= start_page && p_idx < end_page) {
                list_del(&page->list);
                pmm_free_page((void*)page->frame_phys);
                kfree(page);
            }
        }
    }
    
    spinlock_release(&node->pages_lock);
}

void vfs_cache_clear(vfs_node_t *node) {
    if (!node) return;
    
    spinlock_acquire(&node->pages_lock);
    for (int i = 0; i < 32; i++) {
        vfs_page_t *page, *n;
        list_for_each_entry_safe(page, n, &node->pages[i], list) {
            list_del(&page->list);
            pmm_free_page((void*)page->frame_phys);
            kfree(page);
        }
    }
    spinlock_release(&node->pages_lock);
}

void vfs_cache_sync(vfs_node_t *node) {
    if (!node || !node->write) return;

    spinlock_acquire(&node->pages_lock);
    for (int i = 0; i < 32; i++) {
        vfs_page_t *page;
        list_for_each_entry(page, &node->pages[i], list) {
            if (page->dirty) {
                // Clear dirty bit first so we don't recurse or double-sync
                page->dirty = false;
                
                // Release lock while doing I/O
                spinlock_release(&node->pages_lock);
                
                // Write back to the filesystem
                node->write(node, page->offset, 4096, (uint8_t*)PHYS_TO_VIRT(page->frame_phys));
                
                // Re-acquire lock to continue traversal
                spinlock_acquire(&node->pages_lock);
                
                // NOTE: We don't use list_for_each_entry_safe here because we are not
                // deleting the current entry. But we released the lock, so the list
                // could have changed. To be safe, we should restart or use a smarter 
                // marker. For now, we'll just restart the bucket if we had to yield.
                // However, since we cleared the dirty bit, we will eventually finish.
                i--; break; // Optimization: re-check bucket after yielding
            }
        }
    }
    spinlock_release(&node->pages_lock);
}
