/*
 * Unit test for the connection pool.
 *
 * Uses socketpairs and TCP pairs to exercise:
 *   - partial reads return 0
 *   - full message returns 1 and NUL-terminates
 *   - oversize length returns -1
 *   - peer close returns -1
 *   - idle expiry closes old connections
 *   - pool_flush drains a queued frame to the peer
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

#include "connection_pool.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); passed++; } \
    else      { printf("FAIL: %s\n", msg); failed++; } \
} while (0)

static int make_pair(int *out_a, int *out_b)
{
    int sv[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
    {
        return 0;
    }

    *out_a = sv[0];
    *out_b = sv[1];

    return 1;
}

/*
 * Create a real connected TCP pair over loopback. Used by tests
 * that need to observe bytes reaching the peer, which a plain
 * socketpair cannot easily do with small SO_SNDBUF.
 */
static int make_tcp_pair(int *out_server, int *out_client)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);

    if (listener < 0)
    {
        return 0;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listener,
             (struct sockaddr *)&addr,
             sizeof(addr)) < 0)
    {
        close(listener);
        return 0;
    }

    if (listen(listener, 1) < 0)
    {
        close(listener);
        return 0;
    }

    socklen_t len = sizeof(addr);

    if (getsockname(listener,
                    (struct sockaddr *)&addr,
                    &len) < 0)
    {
        close(listener);
        return 0;
    }

    int client = socket(AF_INET, SOCK_STREAM, 0);

    if (client < 0)
    {
        close(listener);
        return 0;
    }

    if (connect(client,
                (struct sockaddr *)&addr,
                sizeof(addr)) < 0)
    {
        close(client);
        close(listener);
        return 0;
    }

    int server = accept(listener, NULL, NULL);
    close(listener);

    if (server < 0)
    {
        close(client);
        return 0;
    }

    *out_server = server;
    *out_client = client;

    return 1;
}
static void test_partial_then_full(void)
{
    int a = -1, b = -1;

    if (!make_pair(&a, &b))
    {
        CHECK(0, "socketpair setup");
        return;
    }

    struct ConnectionPool pool;
    pool_init(&pool);

    int idx = pool_add(&pool, a);
    CHECK(idx >= 0, "pool_add accepted fd");

    struct Connection *c = &pool.conns[idx];

    uint32_t net_len = htonl(5);
    const char *len_bytes = (const char *)&net_len;

    write(b, len_bytes, 2);

    int step = pool_step(c);
    CHECK(step == 0, "partial header returns 0");
    CHECK(c->received == 2, "received 2 bytes");

    write(b, len_bytes + 2, 2);
    write(b, "hello", 5);

    step = pool_step(c);
    CHECK(step == 1, "complete message returns 1");
    CHECK(c->expected == 9, "expected total = 4 + 5");
    CHECK(strcmp(c->buffer + 4, "hello") == 0,
          "payload NUL-terminated");

    pool_remove(&pool, idx);
    close(b);
}

static void test_oversize_length(void)
{
    int a = -1, b = -1;

    if (!make_pair(&a, &b))
    {
        CHECK(0, "socketpair setup");
        return;
    }

    struct ConnectionPool pool;
    pool_init(&pool);

    int idx = pool_add(&pool, a);
    struct Connection *c = &pool.conns[idx];

    uint32_t net_len = htonl(NETWORK_MAX_MESSAGE_SIZE + 1);
    write(b, &net_len, 4);
    write(b, "x", 1);

    int step = pool_step(c);
    CHECK(step == -1, "oversize length returns -1");

    pool_remove(&pool, idx);
    close(b);
}

static void test_peer_close(void)
{
    int a = -1, b = -1;

    if (!make_pair(&a, &b))
    {
        CHECK(0, "socketpair setup");
        return;
    }

    struct ConnectionPool pool;
    pool_init(&pool);

    int idx = pool_add(&pool, a);
    struct Connection *c = &pool.conns[idx];

    close(b);

    int step = pool_step(c);
    CHECK(step == -1, "peer close returns -1");

    pool_remove(&pool, idx);
}

static void test_idle_expiry(void)
{
    int a = -1, b = -1;

    if (!make_pair(&a, &b))
    {
        CHECK(0, "socketpair setup");
        return;
    }

    struct ConnectionPool pool;
    pool_init(&pool);

    int idx = pool_add(&pool, a);
    CHECK(idx >= 0, "pool_add for idle test");
    CHECK(pool.count == 1, "count is 1 after add");

    pool.conns[idx].last_activity =
        time(NULL) - CONNECTION_IDLE_TIMEOUT_SECONDS - 1;

    int expired = pool_expire_idle(&pool);
    CHECK(expired == 1, "one connection expired");
    CHECK(pool.count == 0, "count is 0 after expiry");

    close(b);
}

/*
 * Verify that pool_flush drains a queued frame to a real peer,
 * with the correct 4-byte BE length prefix and payload bytes.
 *
 * The frame is written into the send buffer directly, so this
 * tests pool_flush in isolation from pool_send_message's
 * framing logic.
 */
static void test_flush_drains_pending(void)
{
    int server = -1, client = -1;

    if (!make_tcp_pair(&server, &client))
    {
        CHECK(0, "TCP pair setup for flush test");
        return;
    }

    struct ConnectionPool pool;
    pool_init(&pool);

    int idx = pool_add(&pool, server);

    if (idx < 0)
    {
        CHECK(0, "pool_add for flush test");
        close(client);
        return;
    }

    struct Connection *c = &pool.conns[idx];

    const char *msg = "HELLO_WORLD_TEST_MESSAGE";
    size_t msg_len = strlen(msg);

    uint32_t net_len = htonl((uint32_t)msg_len);
    memcpy(c->send_buffer, &net_len, 4);
    memcpy(c->send_buffer + 4, msg, msg_len);
    c->send_length = 4 + msg_len;
    c->send_offset = 0;

    if (!pool_flush(c))
    {
        CHECK(0, "pool_flush on queued message");
    }
    else
    {
        CHECK(1, "pool_flush on queued message succeeded");
    }

    CHECK(c->send_offset == c->send_length,
          "send buffer fully drained");

    char read_buf[128];
    size_t total = 0;

    while (total < c->send_length)
    {
        ssize_t n = recv(client,
                         read_buf + total,
                         sizeof(read_buf) - total,
                         0);
        if (n <= 0)
        {
            break;
        }
        total += (size_t)n;
    }

    CHECK(total == c->send_length, "peer received all bytes");

    if (total >= 4)
    {
        uint32_t got_len = 0;
        memcpy(&got_len, read_buf, 4);
        got_len = ntohl(got_len);

        CHECK(got_len == msg_len,
              "peer received correct length prefix");

        CHECK(memcmp(read_buf + 4, msg, msg_len) == 0,
              "peer received correct payload");
    }

    pool_remove(&pool, idx);
    close(client);
}

int main(void)
{
    test_partial_then_full();
    test_oversize_length();
    test_peer_close();
    test_idle_expiry();
    test_flush_drains_pending();
        printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
