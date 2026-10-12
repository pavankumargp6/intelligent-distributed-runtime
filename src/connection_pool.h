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
    size_t expected;

    /*
     * Outbound bytes. Filled by pool_send_message and drained
     * by pool_flush across possibly several poll cycles. The
     * frame is the same 4 + payload layout as the read path.
     */
    char send_buffer[4 + NETWORK_MAX_MESSAGE_SIZE];
    size_t send_length;
    size_t send_offset;

    /*
     * Set once the request has been fully processed and the
     * response fully queued. pool_flush closes the connection
     * when the response is fully drained and this flag is set.
     */
    int closing_after_flush;

    time_t last_activity;
};

struct ConnectionPool
{
    struct Connection conns[CONNECTION_POOL_CAPACITY];
    int count;
};

void pool_init(struct ConnectionPool *pool);

int pool_add(struct ConnectionPool *pool, int fd);

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
 * Queue a response on the connection and attempt to send it.
 *
 * The frame is built as [4-byte BE length][payload] and
 * appended to the connection's send buffer. Any bytes that
 * cannot be written immediately (EAGAIN) remain pending and
 * are flushed by pool_flush when the socket becomes writable.
 *
 * Only one message may be pending at a time. Attempting to
 * queue a second message while the first is still pending
 * returns 0.
 *
 * Returns 1 if the message was queued (whether or not it was
 * fully sent immediately). Returns 0 on error; the caller
 * should remove the connection.
 */
int pool_send_message(struct Connection *conn, const char *message);

/*
 * Attempt to drain any pending outbound bytes.
 *
 * Returns 1 if the connection is still healthy (whether or
 * not bytes remain). Returns 0 on fatal error; the caller
 * should remove the connection.
 */
int pool_flush(struct Connection *conn);

/*
 * Close connections whose last_activity is older than
 * CONNECTION_IDLE_TIMEOUT_SECONDS. Returns how many were closed.
 */
int pool_expire_idle(struct ConnectionPool *pool);

/*
 * Fill pollfds[1..] with the live connections. Each connection
 * gets POLLIN, plus POLLOUT if it has pending outbound bytes.
 * pollfds[0] must already contain the listener. Returns the
 * total number of fds written (including the listener slot).
 */
int pool_build_pollfds(struct ConnectionPool *pool,
                       struct pollfd *pollfds,
                       int capacity);

#endif
