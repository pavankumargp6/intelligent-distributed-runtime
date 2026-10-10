/*
 * Unit test for the connection pool.
 *
 * Uses a connected socketpair to exercise:
 *   - partial reads return 0
 *   - full message returns 1 and NUL-terminates
 *   - oversize length returns -1
 *   - peer close returns -1
 *   - idle expiry closes old connections
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

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

    /* Send only 2 bytes of the 4-byte header. */
    uint32_t net_len = htonl(5);
    const char *len_bytes = (const char *)&net_len;

    write(b, len_bytes, 2);

    int step = pool_step(c);
    CHECK(step == 0, "partial header returns 0");
    CHECK(c->received == 2, "received 2 bytes");

    /* Send the rest of the header plus the payload. */
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

    /* Claim a payload larger than NETWORK_MAX_MESSAGE_SIZE. */
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

    /* Backdate last_activity past the timeout. */
    pool.conns[idx].last_activity =
        time(NULL) - CONNECTION_IDLE_TIMEOUT_SECONDS - 1;

    int expired = pool_expire_idle(&pool);
    CHECK(expired == 1, "one connection expired");
    CHECK(pool.count == 0, "count is 0 after expiry");

    close(b);
}

int main(void)
{
    test_partial_then_full();
    test_oversize_length();
    test_peer_close();
    test_idle_expiry();

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
