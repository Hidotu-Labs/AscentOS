// AF_UNIX – Bound-socket address registry
//
// Owns the global unix_bound_list / unix_bound_lock and provides the two
// address-lookup helpers used by the rest of the subsystem.

#include "af_unix_internal.h"

// ── Global registry ───────────────────────────────────────────────────────────

struct list_head unix_bound_list;
spinlock_t       unix_bound_lock;

// ── Lookup ────────────────────────────────────────────────────────────────────

unix_sock_t *unix_find_socket_by_addr(struct sockaddr_un *addr, int addrlen) {
  struct list_head *pos;

  spinlock_acquire(&unix_bound_lock);

  list_for_each(pos, &unix_bound_list) {
    unix_sock_t *usk = list_entry(pos, unix_sock_t, bind_node);

    if (usk->addr.sun_family != AF_UNIX)
      continue;

    if (usk->addr.sun_path[0] == '\0') {
      // Abstract socket: match exact length and bytes
      if (usk->addr_len != addrlen)
        continue;
      if (memcmp(usk->addr.sun_path, addr->sun_path,
                 addrlen - offsetof(struct sockaddr_un, sun_path)) == 0) {
        spinlock_release(&unix_bound_lock);
        return usk;
      }
    } else {
      // Filesystem socket: compare path strings
      if (strcmp(usk->addr.sun_path, addr->sun_path) == 0) {
        spinlock_release(&unix_bound_lock);
        return usk;
      }
    }
  }

  spinlock_release(&unix_bound_lock);
  return NULL;
}

/**
 * Mark a socket's filesystem entry as unlinked.
 * Called when a socket file is unlinked via VFS.
 * The socket stays in the bound list so re-binding fails with EADDRINUSE.
 * Returns 0 on success, -1 if not found.
 */
int unix_unbind_by_path(const char *path) {
  if (!path || path[0] == '\0')
    return -1;

  struct list_head *pos, *n;
  int found = 0;

  spinlock_acquire(&unix_bound_lock);

  list_for_each_safe(pos, n, &unix_bound_list) {
    unix_sock_t *usk = list_entry(pos, unix_sock_t, bind_node);

    if (usk->addr.sun_path[0] == '\0')
      continue; // skip abstract sockets

    if (strcmp(usk->addr.sun_path, path) == 0) {
      // Clear the VFS node pointer – file is gone, but keep in bound list
      // so EADDRINUSE is returned for any re-bind attempt.
      usk->parent->node = NULL;
      found = 1;
      break;
    }
  }

  spinlock_release(&unix_bound_lock);
  return found ? 0 : -1;
}
