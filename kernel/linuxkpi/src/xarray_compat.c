/* Support symbols that upstream's lib/xarray.c expects from lib/radix-tree.c.
 *
 * Upstream keeps the exported radix-tree node cache and its RCU free callback
 * in the (deprecated) radix-tree translation unit.  AvoryOS does not import
 * that file yet -- it drags in timers/hrtimer/sched infrastructure that lands
 * later -- so the two symbols xarray actually consumes are provided here
 * with the same semantics:
 *   - radix_tree_node_cachep is also used by drivers (e.g. DRM) for xarray
 *     node allocation;
 *   - the free callback clears slots/tags and re-initialises private_list
 *     before returning the node to the cache (upstream does the same, so
 *     zeroed nodes are cached).
 * The full radix-tree.c import (and the compat iteration helpers IDR needs)
 * happens after the timer/workqueue phase.
 */

#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/xarray.h>

struct kmem_cache *radix_tree_node_cachep;

void radix_tree_node_rcu_free(struct rcu_head *head) {
  struct xa_node *node = container_of(head, struct xa_node, rcu_head);

  __builtin_memset(node->slots, 0, sizeof(node->slots));
  __builtin_memset(node->tags, 0, sizeof(node->tags));
  INIT_LIST_HEAD(&node->private_list);

  kmem_cache_free(radix_tree_node_cachep, node);
}

/* Called once during LinuxKPI bring-up, before any xarray is used. */
void linuxkpi_xarray_init(void) {
  if (!radix_tree_node_cachep)
    radix_tree_node_cachep = kmem_cache_create("radix_tree_node",
                                               sizeof(struct xa_node), 0, 0,
                                               NULL);
}
