#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include <time.h>
#include <poll.h>

#include "network_protocol.h"
#include "auth.h"

#define CONNECTION_POOL_CAPACITY 64
#define CONNECTION_IDLE_TIMEOUT_SECONDS 5

typedef enum
{
    CONN_ROLE_UNSET = 0,
    CONN_ROLE_WORKER,
    CONN_ROLE_CLIENT
} ConnRole;

struct Connection
{
    int fd;

    /*
     * Read buffer for one message. Layout:
     *   bytes [0..3]        4-byte network-order length
     *   bytes [4..4+len-1]  payload
     *   byte  [4+len]       NUL, written when the message is complete
     */
    char buffer[4 + NETWORK_MAX_MESSAGE_SIZE + 1];
    size_t received;
    size_t expected;

    /*
     * Outbound bytes. Filled by pool_send_message and drained
     * by pool_flush across possibly several poll cycles.
     */
    char send_buffer[4 + NETWORK_MAX_MESSAGE_SIZE];
    size_t send_length;
    size_t send_offset;

    int closing_after_flush;

    time_t last_activity;

    /*
     * Authentication state for the peer on this connection.
     *
     * peer_role is set only after successful MAC verification
     * of the request (or, in insecure mode, after the message
     * type is identified). Response signing uses
     * response_secret and response_direction; when
     * response_secret is NULL, pool_send_message sends the
     * response unmodified.
     *
     * A connection carries at most one request/response pair,
     * so these are set once and never change.
     */
    ConnRole peer_role;
    const char *response_secret;
    AuthDirection response_direction;
};

struct ConnectionPool
{
    struct Connection conns[CONNECTION_POOL_CAPACITY];
    int count;
};

void pool_init(struct ConnectionPool *pool);

int pool_add(struct ConnectionPool *pool, int fd);

void pool_remove(struct ConnectionPool *pool, int index);

int pool_step(struct Connection *conn);

/*
 * Queue a response on the connection.
 *
 * If conn->response_secret is non-NULL, the message is first
 * signed with HMAC-SHA256 using the given secret and
 * direction, with the signer role fixed at
 * AUTH_ROLE_COORDINATOR. Otherwise the message is sent
 * unmodified. In both cases the framed bytes are appended to
 * the send buffer and pool_flush is called.
 *
 * Returns 1 on success, 0 on error; the caller should remove
 * the connection.
 */
int pool_send_message(struct Connection *conn, const char *message);

int pool_flush(struct Connection *conn);

int pool_expire_idle(struct ConnectionPool *pool);

int pool_build_pollfds(struct ConnectionPool *pool,
                       struct pollfd *pollfds,
                       int capacity);

#endif
