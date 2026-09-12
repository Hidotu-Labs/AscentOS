#ifndef LINUXKPI_NATIVE_MM_H
#define LINUXKPI_NATIVE_MM_H

/* Native memory bridge for LinuxKPI implementation files.
 *
 * Native functions are declared under a distinct C name with an
 * __asm__("native_symbol") alias: the implementation can then call
 * asc_kmalloc() without colliding with the Linux kmalloc() API it is
 * implementing.  Types use compiler builtins (__SIZE_TYPE__) instead of
 * <stddef.h>/<stdint.h> so this header cannot conflict with Linux headers.
 */

extern void *asc_kmalloc(__SIZE_TYPE__ size) __asm__("kmalloc");
extern void asc_kfree(void *ptr) __asm__("kfree");
extern void *asc_kcalloc(__SIZE_TYPE__ num, __SIZE_TYPE__ size)
    __asm__("kcalloc");
extern void *asc_krealloc(void *ptr, __SIZE_TYPE__ new_size)
    __asm__("krealloc");

/* Native named object caches (kernel/src/mm/slab_cache.{c,h}).
 * kmem_cache_create(name, obj_size, alignment, ctor, dtor). */
extern void *asc_kmem_cache_create(const char *name, __SIZE_TYPE__ obj_size,
                                   __SIZE_TYPE__ alignment,
                                   void (*ctor)(void *),
                                   void (*dtor)(void *))
    __asm__("kmem_cache_create");
extern void *asc_kmem_cache_alloc(void *cache) __asm__("kmem_cache_alloc");
extern void asc_kmem_cache_free(void *cache, void *obj)
    __asm__("kmem_cache_free");
extern void asc_kmem_cache_destroy(void *cache) __asm__("kmem_cache_destroy");
extern void asc_kmem_cache_shrink(void *cache) __asm__("kmem_cache_shrink");

#endif /* LINUXKPI_NATIVE_MM_H */
