#ifndef AF_UNIX_INTERNAL_H
#define AF_UNIX_INTERNAL_H

// Internal header shared between af_unix_*.c sub-modules.
// Do NOT include from outside the socket/ directory.

#include "af_unix.h"
#include "../console/klog.h"
#include "../fs/vfs.h"
#include "../lib/list.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "epoll.h"
#include "socket.h"
#include "socket_internal.h"
#include <stddef.h>
#include <stdint.h>

// ── Bound-socket registry (owned by af_unix_addr.c) ──────────────────────────
extern struct list_head unix_bound_list;
extern spinlock_t       unix_bound_lock;

// ── Internal function declarations ───────────────────────────────────────────

// af_unix_addr.c
unix_sock_t *unix_find_socket_by_addr(struct sockaddr_un *addr, int addrlen);
unix_sock_t *unix_find_socket_by_addr_ref(struct sockaddr_un *addr, int addrlen);

// af_unix_bind.c  (unix_bind is the public ops entry — kept static there)

// af_unix_connect.c
// unix_connect / unix_accept / unix_listen are static, registered via unix_ops

// af_unix_io.c
// all send/recv variants are static, registered via unix_ops

// af_unix_sockopt.c
// getsockopt / setsockopt are static, registered via unix_ops

#endif // AF_UNIX_INTERNAL_H
