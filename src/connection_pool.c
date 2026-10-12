#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "connection_pool.h"
#include "log.h"

void pool_init(struct ConnectionPool *pool)
{
    if (pool == NULL)
    {
        return;
    }

    memset(pool, 0, sizeof(*pool));

    for (int i = 0; i < CONNECTION_POOL_CAPACITY; i++)
    {
        pool->conns[i].fd = -1;
    }
}

int pool_add(struct ConnectionPool *pool, int fd)
{
    if (pool == NULL || fd < 0)
    {
        return -1;
    }

    if (pool->count >= CONNECTION_POOL_CAPACITY)
    {
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        return -1;
    }

    for (int i = 0; i < CONNECTION_POOL_CAPACITY; i++)
    {
        if (pool->conns[i].fd == -1)
        {
            struct Connection *c = &pool->conns[i];
            memset(c, 0, sizeof(*c));
            c->fd = fd;
            c->received = 0;
            c->expected = 0;
            c->send_length = 0;
            c->send_offset = 0;
            c->closing_after_flush = 0;
            c->last_activity = time(NULL);
            pool->count++;
            return i;
        }
    }

    return -1;
}

void pool_remove(struct ConnectionPool *pool, int index)
{
    if (pool == NULL ||
        index < 0 ||
        index >= CONNECTION_POOL_CAPACITY)
    {
        return;
    }

    struct Connection *c = &pool->conns[index];

    if (c->fd >= 0)
    {
        close(c->fd);
        c->fd = -1;
    }

    c->received = 0;
    c->expected = 0;
    c->send_length = 0;
    c->send_offset = 0;
    c->closing_after_flush = 0;

    if (pool->count > 0)
    {
        pool->count--;
    }
}

int pool_step(struct Connection *conn)
{
    if (conn == NULL || conn->fd < 0)
    {
        return -1;
    }

    /*
     * If we've already dispatched the request and are only
     * waiting to flush the response, do not read more bytes.
     */
    if (conn->closing_after_flush)
    {
        return 0;
    }

    size_t space = sizeof(conn->buffer) - 1 - conn->received;

    if (space == 0)
    {
        return -1;
    }

    ssize_t n = recv(conn->fd,
                     conn->buffer + conn->received,
                     space,
                     0);

    if (n == 0)
    {
        return -1;
    }

    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            /* No data right now. */
        }
        else if (errno == EINTR)
        {
            /* Interrupted; not fatal. */
        }
        else
        {
            return -1;
        }
    }
    else
    {
        conn->received += (size_t)n;
        conn->last_activity = time(NULL);
    }

    if (conn->received < 4)
    {
        return 0;
    }

    if (conn->expected == 0)
    {
        uint32_t network_length = 0;
        memcpy(&network_length, conn->buffer, 4);
        uint32_t payload_length = ntohl(network_length);

        if (payload_length > NETWORK_MAX_MESSAGE_SIZE)
        {
            return -1;
        }

        conn->expected = 4 + (size_t)payload_length;
    }

    if (conn->received < conn->expected)
    {
        return 0;
    }

    conn->buffer[conn->expected] = '\0';
    return 1;
}

int pool_send_message(struct Connection *conn, const char *message)
{
    if (conn == NULL || conn->fd < 0 || message == NULL)
    {
        return 0;
    }

    if (conn->send_offset < conn->send_length)
    {
        log_warn("pool_send_while_pending",
                 "Connection already has %zu bytes pending",
                 conn->send_length - conn->send_offset);
        return 0;
    }

    size_t message_length = strlen(message);

    if (message_length > NETWORK_MAX_MESSAGE_SIZE)
    {
        return 0;
    }

    uint32_t network_length = htonl((uint32_t)message_length);

    memcpy(conn->send_buffer, &network_length, 4);
    memcpy(conn->send_buffer + 4, message, message_length);

    conn->send_length = 4 + message_length;
    conn->send_offset = 0;

    return pool_flush(conn);
}

int pool_flush(struct Connection *conn)
{
    if (conn == NULL || conn->fd < 0)
    {
        return 0;
    }

    while (conn->send_offset < conn->send_length)
    {
        ssize_t n = send(conn->fd,
                         conn->send_buffer + conn->send_offset,
                         conn->send_length - conn->send_offset,
                         MSG_NOSIGNAL);

        if (n > 0)
        {
            conn->send_offset += (size_t)n;
            continue;
        }

        if (n == 0)
        {
            return 0;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 1;
        }

        if (errno == EINTR)
        {
            continue;
        }

        return 0;
    }

    return 1;
}

int pool_expire_idle(struct ConnectionPool *pool)
{
    if (pool == NULL)
    {
        return 0;
    }

    time_t now = time(NULL);
    int expired = 0;

    for (int i = 0; i < CONNECTION_POOL_CAPACITY; i++)
    {
        struct Connection *c = &pool->conns[i];

        if (c->fd < 0)
        {
            continue;
        }

        double elapsed = difftime(now, c->last_activity);

        if (elapsed > CONNECTION_IDLE_TIMEOUT_SECONDS)
        {
            pool_remove(pool, i);
            expired++;
        }
    }

    return expired;
}

int pool_build_pollfds(struct ConnectionPool *pool,
                       struct pollfd *pollfds,
                       int capacity)
{
    if (pool == NULL || pollfds == NULL || capacity < 1)
    {
        return 0;
    }

    int n = 1;

    for (int i = 0;
         i < CONNECTION_POOL_CAPACITY && n < capacity;
         i++)
    {
        if (pool->conns[i].fd < 0)
        {
            continue;
        }

        short events = POLLIN;

        if (pool->conns[i].send_offset <
            pool->conns[i].send_length)
        {
            events |= POLLOUT;
        }

        pollfds[n].fd = pool->conns[i].fd;
        pollfds[n].events = events;
        pollfds[n].revents = 0;
        n++;
    }

    return n;
}
