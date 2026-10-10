#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "connection_pool.h"

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

    /*
     * Make the fd non-blocking. accept4() with SOCK_NONBLOCK is
     * another option, but doing it here keeps the pool as the
     * single place that decides connection I/O semantics.
     */
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
     * Try to read. Leave room for a trailing NUL.
     */
    size_t space = sizeof(conn->buffer) - 1 - conn->received;

    if (space == 0)
    {
        /* Buffer full without a complete message; protocol error. */
        return -1;
    }

    ssize_t n = recv(conn->fd,
                     conn->buffer + conn->received,
                     space,
                     0);

    if (n == 0)
    {
        /* Peer closed. */
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
            /* Interrupted; not fatal. Try again later. */
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

    /*
     * Slot 0 is reserved for the listener; the caller fills it in
     * before calling us. We start filling at index 1.
     */
    int n = 1;

    for (int i = 0;
         i < CONNECTION_POOL_CAPACITY && n < capacity;
         i++)
    {
        if (pool->conns[i].fd < 0)
        {
            continue;
        }

        pollfds[n].fd = pool->conns[i].fd;
        pollfds[n].events = POLLIN;
        pollfds[n].revents = 0;
        n++;
    }

    return n;
}
