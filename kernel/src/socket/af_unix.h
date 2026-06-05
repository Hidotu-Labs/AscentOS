#ifndef AF_UNIX_H
#define AF_UNIX_H

#include "socket.h"

// AF_UNIX Family Registration

/**
 * Initialize and register the AF_UNIX socket family.
 * Called during socket subsystem initialization.
 */
void af_unix_init(void);

// AF_UNIX Socket Operations

/**
 * Create an AF_UNIX socket.
 * @param sock The socket structure to initialize
 * @param protocol Protocol number (must be 0 for AF_UNIX)
 * @return 0 on success, negative error code on failure
 */
int unix_create(socket_t *sock, int protocol);

/**
 * Destroy an AF_UNIX socket.
 * @param sock The socket to destroy
 */
void unix_destroy(socket_t *sock);

/**
 * Unbind a socket by its filesystem path.
 * Called when a socket file is unlinked from the filesystem.
 * @param path The filesystem path of the socket to unbind
 * @return 0 on success, -1 if not found
 */
int unix_unbind_by_path(const char *path);

// AF_UNIX Operations Vector

/**
 * Get the AF_UNIX socket operations vector.
 * @return Pointer to the sock_ops structure for AF_UNIX
 */
sock_ops_t *unix_get_ops(void);

// AF_UNIX Family Structure

/**
 * Get the AF_UNIX net_family structure for registration.
 * @return Pointer to the net_family structure
 */
net_family_t *unix_get_family(void);

#endif // AF_UNIX_H
