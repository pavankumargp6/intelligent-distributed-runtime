#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include <time.h>
#include <poll.h>

#include "network_protocol.h"

#define CONNECTION_POOL_CAPACITY 64
#define CONNECTION_IDLE_TIMEOUT_SECONDS 5

struct Connection
{
    int fd;

    /*
     * Read buffer for one message. Layout:
     *   bytes [0..3]        4-byte network-order length
     *   bytes [4..4+len-1]  payload
     *   byte  [4+len]       NUL, written when the message is complete
     *
     * The +1 guarantees room for the terminator.
     */
    char buffer[4 + NETWORK_MAX_MESSAGE_SIZE + 1];
    size_t received;

    /*
     * Total bytes expected for the current message once the
     * length header has been parsed. Zero while the header is
     * still incomplete.
     */
    size_t expected;

    time_t last_activity;
};

struct ConnectionPool
{
    struct Connection conns[CONNECTION_POOL_CAPACITY];
    int count;
};

void pool_init(struct ConnectionPool *pool);

/*
 * Add an accepted fd. Returns the slot index, or -1 if full.
 * Sets the fd non-blocking.
 */
int pool_add(struct ConnectionPool *pool, int fd);

/* Remove a connection by slot index and close its fd. */
void pool_remove(struct ConnectionPool *pool, int index);

/*
 * Read available bytes from one connection.
 *
 * Returns:
 *   -1  fatal (peer closed, recv error, oversize length)
 *    0  more data needed
 *    1  full message in conn->buffer, NUL-terminated at
 *       conn->expected
 */
int pool_step(struct Connection *conn);

/*
 * Close connections whose last_activity is older than
 * CONNECTION_IDLE_TIMEOUT_SECONDS. Returns how many were closed.
 */
int pool_expire_idle(struct ConnectionPool *pool);

/*
 * Fill pollfds[1..] with the live connections (POLLIN each).
 * pollfds[0] must already contain the listener. Returns the
 * total number of fds written (including the listener slot).
 */
int pool_build_pollfds(struct ConnectionPool *pool,
                       struct pollfd *pollfds,
                       int capacity);

#endif
