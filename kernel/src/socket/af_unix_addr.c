// AF_UNIX – Bound-socket address registry
//
// Owns the global unix_bound_list / unix_bound_lock and provides the two
// address-lookup helpers used by the rest of the subsystem.

#include "af_unix_internal.h"

// ── Global registry ───────────────────────────────────────────────────────────

struct list_head unix_bound_list;
spinlock_t       unix_bound_lock;

// ── Lookup ────────────────────────────────────────────────────────────────────

static size_t unix_fs_path_len(const struct sockaddr_un *addr, int addrlen) {
  if (addrlen <= (int)offsetof(struct sockaddr_un, sun_path))
    return 0;

  size_t len = (size_t)(addrlen - offsetof(struct sockaddr_un, sun_path));
  if (len > sizeof(addr->sun_path))
    len = sizeof(addr->sun_path);

  // Linux callers may omit the trailing NUL from addrlen; ignore a final NUL.
  if (len > 0 && addr->sun_path[len - 1] == '\0')
    len--;

  return len;
}

static bool unix_fs_paths_equal(const struct sockaddr_un *a, int alen,
                                const struct sockaddr_un *b, int blen) {
  size_t a_len = unix_fs_path_len(a, alen);
  size_t b_len = unix_fs_path_len(b, blen);

  if (a_len != b_len)
    return false;
  if (a_len == 0)
    return true;
  return memcmp(a->sun_path, b->sun_path, a_len) == 0;
}

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
    } else if (unix_fs_paths_equal(&usk->addr, usk->addr_len, addr, addrlen)) {
      spinlock_release(&unix_bound_lock);
      return usk;
    }
  }

  spinlock_release(&unix_bound_lock);
  return NULL;
}

unix_sock_t *unix_find_socket_by_addr_ref(struct sockaddr_un *addr, int addrlen) {
  struct list_head *pos;

  spinlock_acquire(&unix_bound_lock);

  list_for_each(pos, &unix_bound_list) {
    unix_sock_t *usk = list_entry(pos, unix_sock_t, bind_node);
    bool match = false;

    if (usk->addr.sun_family != AF_UNIX)
      continue;

    if (usk->addr.sun_path[0] == '\0') {
      if (usk->addr_len == addrlen &&
          memcmp(usk->addr.sun_path, addr->sun_path,
                 addrlen - offsetof(struct sockaddr_un, sun_path)) == 0)
        match = true;
    } else if (unix_fs_paths_equal(&usk->addr, usk->addr_len, addr, addrlen)) {
      match = true;
    }

    if (match) {
      socket_t *parent = usk->parent;
      if (parent && socket_try_get(parent)) {
        spinlock_release(&unix_bound_lock);
        return usk;
      }
      break;
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
