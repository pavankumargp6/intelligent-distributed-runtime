/*
 * Unit test for src/wal.c.
 *
 * Exercises append / read round-trip, missing-file behaviour,
 * and rejection of lines containing newlines.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include "wal.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); passed++; } \
    else      { printf("FAIL: %s\n", msg); failed++; } \
} while (0)

static char g_path[256];

static int collect_cb(const char *line, void *user)
{
    char (*buf)[256] = user;
    static int idx = 0;

    strncpy(buf[idx], line, 255);
    buf[idx][255] = '\0';
    idx++;

    return 1;
}

static int stop_early_cb(const char *line, void *user)
{
    (void)line;
    (void)user;
    return 0;
}

static void test_roundtrip(void)
{
    unlink(g_path);

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_path))
    {
        CHECK(0, "wal_open_for_append");
        return;
    }
    CHECK(1, "wal_open_for_append");

    if (!wal_append(&wal, "SUBMIT|1|4:echo|1|5:hello"))
    {
        CHECK(0, "append 1");
    }
    if (!wal_append(&wal, "SUBMIT|2|5:sleep|1|2:30"))
    {
        CHECK(0, "append 2");
    }
    if (!wal_append(&wal, "RESULT|1|1|0|2"))
    {
        CHECK(0, "append 3");
    }

    CHECK(1, "three appends succeeded");

    wal_close(&wal);

    static char collected[16][256];
    memset(collected, 0, sizeof(collected));

    int count = wal_foreach_line(g_path, collect_cb, collected);

    CHECK(count == 3, "foreach_line returns 3");

    CHECK(strcmp(collected[0], "SUBMIT|1|4:echo|1|5:hello") == 0,
          "line 0 content");
    CHECK(strcmp(collected[1], "SUBMIT|2|5:sleep|1|2:30") == 0,
          "line 1 content");
    CHECK(strcmp(collected[2], "RESULT|1|1|0|2") == 0,
          "line 2 content");
}

static void test_missing_file(void)
{
    char missing[256];
    snprintf(missing, sizeof(missing),
             "/tmp/idr_wal_test_missing_%d.log", (int)getpid());
    unlink(missing);

    int count = wal_foreach_line(missing, collect_cb, NULL);

    CHECK(count == 0, "missing file returns 0, not error");
}

static void test_reject_newline(void)
{
    unlink(g_path);

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_path))
    {
        CHECK(0, "reject-newline: open");
        return;
    }

    int result = wal_append(&wal, "line\nwith\nnewline");

    CHECK(result == 0, "append rejects embedded newline");

    wal_close(&wal);
    unlink(g_path);
}

static void test_stop_early(void)
{
    unlink(g_path);

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_path))
    {
        CHECK(0, "stop-early: open");
        return;
    }

    wal_append(&wal, "one");
    wal_append(&wal, "two");
    wal_append(&wal, "three");
    wal_close(&wal);

    /* Callback returns 0 on first line, so iteration stops. */
    int count = wal_foreach_line(g_path, stop_early_cb, NULL);

    CHECK(count == 1, "stop-early: one line then stop");

    unlink(g_path);
}

int main(void)
{
    snprintf(g_path, sizeof(g_path),
             "/tmp/idr_wal_test_%d.log", (int)getpid());

    test_roundtrip();
    test_missing_file();
    test_reject_newline();
    test_stop_early();

    unlink(g_path);

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
