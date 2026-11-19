/*
 * Unit tests for the WAL v2 engine.
 *
 * Covers framing, CRC validation, LSN assignment and filtering,
 * segment rotation, torn-tail truncation, mid-file corruption,
 * segment cleanup, and legacy-file behaviour.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wal.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); passed++; } \
    else      { printf("FAIL: %s\n", msg); failed++; } \
} while (0)

static char g_base[512];

static int remove_wal_files(const char *base)
{
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s*", base);

    glob_t matches;

    memset(&matches, 0, sizeof(matches));

    if (glob(pattern, 0, NULL, &matches) == 0)
    {
        for (size_t i = 0; i < matches.gl_pathc; i++)
        {
            unlink(matches.gl_pathv[i]);
        }
    }

    globfree(&matches);

    return 1;
}

struct Collected
{
    size_t count;
    enum WalRecordType types[64];
    uint64_t lsns[64];
    char payloads[64][256];
};

static int collect_cb(enum WalRecordType type,
                      uint64_t lsn,
                      const char *payload,
                      void *user)
{
    struct Collected *collected = user;

    if (collected->count >= 64)
    {
        return 0;
    }

    size_t index = collected->count++;

    collected->types[index] = type;
    collected->lsns[index] = lsn;
    snprintf(collected->payloads[index],
             sizeof(collected->payloads[index]),
             "%s",
             payload);

    return 1;
}

static int count_cb(enum WalRecordType type,
                    uint64_t lsn,
                    const char *payload,
                    void *user)
{
    (void)type;
    (void)lsn;
    (void)payload;

    size_t *count = user;
    (*count)++;

    return 1;
}

static int stop_early_cb(enum WalRecordType type,
                         uint64_t lsn,
                         const char *payload,
                         void *user)
{
    (void)type;
    (void)lsn;
    (void)payload;
    (void)user;
    return 0;
}

static int append_submit(struct Wal *wal, int first, int count)
{
    for (int i = 0; i < count; i++)
    {
        char line[128];

        snprintf(line, sizeof(line),
                 "SUBMIT|%d|4:echo|1|1:x|0|0",
                 first + i);

        if (!wal_append(wal, line))
        {
            return 0;
        }
    }

    return 1;
}

static int count_segments(const char *base)
{
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s.seg.*", base);

    glob_t matches;
    memset(&matches, 0, sizeof(matches));

    int count = 0;

    if (glob(pattern, 0, NULL, &matches) == 0)
    {
        count = (int)matches.gl_pathc;
    }

    globfree(&matches);

    return count;
}

static void test_roundtrip_and_filter(void)
{
    remove_wal_files(g_base);

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_base))
    {
        CHECK(0, "roundtrip: open");
        return;
    }
    CHECK(1, "roundtrip: open creates the first segment");
    CHECK(wal.next_lsn == 1, "roundtrip: first LSN is 1");
    CHECK(count_segments(g_base) == 1, "roundtrip: one segment exists");

    CHECK(wal_append(&wal, "SUBMIT|1|4:echo|1|5:hello|0|0"),
          "roundtrip: append SUBMIT");
    CHECK(wal_append(&wal, "DISPATCH|1|1|0|1|123"),
          "roundtrip: append DISPATCH");
    CHECK(wal_append(&wal, "ACCEPT|1|1|0|1|124"),
          "roundtrip: append ACCEPT");
    CHECK(wal_append(&wal, "RESULT|1|1|0|2"),
          "roundtrip: append RESULT");
    CHECK(wal.next_lsn == 5, "roundtrip: next LSN advanced to 5");

    wal_close(&wal);

    struct Collected collected;
    memset(&collected, 0, sizeof(collected));

    int scanned = wal_foreach_record(g_base,
                                     0,
                                     collect_cb,
                                     &collected);

    CHECK(scanned == 4, "roundtrip: four records scanned");
    CHECK(collected.count == 4, "roundtrip: four records delivered");
    CHECK(collected.types[0] == WAL_RECORD_SUBMIT &&
          collected.types[1] == WAL_RECORD_DISPATCH &&
          collected.types[2] == WAL_RECORD_ACCEPT &&
          collected.types[3] == WAL_RECORD_RESULT,
          "roundtrip: record types preserved");
    CHECK(collected.lsns[0] == 1 && collected.lsns[3] == 4,
          "roundtrip: LSNs are 1..4");
    CHECK(strcmp(collected.payloads[0],
                 "SUBMIT|1|4:echo|1|5:hello|0|0") == 0,
          "roundtrip: payload preserved");

    memset(&collected, 0, sizeof(collected));

    scanned = wal_foreach_record(g_base,
                                 2,
                                 collect_cb,
                                 &collected);

    CHECK(scanned == 4, "filter: all records still validated");
    CHECK(collected.count == 2, "filter: only records after LSN 2 delivered");
    CHECK(collected.lsns[0] == 3 && collected.lsns[1] == 4,
          "filter: delivered LSNs are 3 and 4");

    wal.fd = -1;

    if (!wal_open_for_append(&wal, g_base))
    {
        CHECK(0, "reopen: open");
        return;
    }

    CHECK(wal.next_lsn == 5, "reopen: next LSN continues at 5");
    CHECK(wal_append(&wal, "RESULT|2|1|0|2"),
          "reopen: append after reopen");
    wal_close(&wal);

    memset(&collected, 0, sizeof(collected));
    scanned = wal_foreach_record(g_base, 0, collect_cb, &collected);

    CHECK(scanned == 5, "reopen: five records after second append");
    CHECK(collected.lsns[4] == 5, "reopen: new record got LSN 5");
}

static void test_rotation_and_cleanup(void)
{
    remove_wal_files(g_base);

    setenv("IDR_SEGMENT_BYTES", "4096", 1);

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_base))
    {
        CHECK(0, "rotation: open");
        unsetenv("IDR_SEGMENT_BYTES");
        return;
    }

    CHECK(append_submit(&wal, 1, 200), "rotation: appended 200 records");

    uint64_t active = 0;
    int found = wal_find_active_segment(g_base, &active);

    CHECK(found == 1, "rotation: active segment found");
    CHECK(active > 1, "rotation: more than one segment was created");
    CHECK(count_segments(g_base) > 1, "rotation: multiple segment files");

    wal_close(&wal);

    size_t total = 0;
    int scanned = wal_foreach_record(g_base,
                                     0,
                                     count_cb,
                                     &total);

    CHECK(scanned == 200, "rotation: all 200 records replay in order");
    CHECK(total == 200, "rotation: callback saw all 200 records");

    struct Collected collected;
    memset(&collected, 0, sizeof(collected));

    wal_foreach_record(g_base, 0, collect_cb, &collected);

    CHECK(collected.count == 64,
          "rotation: collection caps at the 64-entry test buffer");

    for (size_t i = 1; i < collected.count; i++)
    {
        if (collected.lsns[i] != collected.lsns[i - 1] + 1)
        {
            CHECK(0, "rotation: LSNs increase by one");
            break;
        }

        if (i + 1 == collected.count)
        {
            CHECK(1, "rotation: LSNs increase by one");
        }
    }

    int removed = wal_remove_segments_before(g_base, active);

    CHECK(removed > 0, "cleanup: removed old segments");
    CHECK(count_segments(g_base) == 1,
          "cleanup: only the active segment remains");

    unsetenv("IDR_SEGMENT_BYTES");
}

static void test_torn_tail(void)
{
    remove_wal_files(g_base);

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_base))
    {
        CHECK(0, "torn: open");
        return;
    }

    CHECK(append_submit(&wal, 1, 3), "torn: appended three records");
    wal_close(&wal);

    char path[600];
    if (!wal_segment_path(g_base, 1, path, sizeof(path)))
    {
        CHECK(0, "torn: segment path");
        return;
    }

    struct stat info;

    if (stat(path, &info) != 0 || info.st_size < 8)
    {
        CHECK(0, "torn: stat segment");
        return;
    }

    if (truncate(path, info.st_size - 5) != 0)
    {
        CHECK(0, "torn: truncate segment");
        return;
    }

    wal.fd = -1;

    if (!wal_open_for_append(&wal, g_base))
    {
        CHECK(0, "torn: reopen after torn tail");
        return;
    }

    CHECK(1, "torn: reopen succeeds");
    CHECK(wal.next_lsn == 3,
          "torn: next LSN rewinds to the first missing record");

    CHECK(wal_append(&wal, "SUBMIT|3|4:echo|1|1:y|0|0"),
          "torn: append after truncation");

    wal_close(&wal);

    struct Collected collected;
    memset(&collected, 0, sizeof(collected));

    int scanned = wal_foreach_record(g_base, 0, collect_cb, &collected);

    CHECK(scanned == 3, "torn: three complete records remain");
    CHECK(strcmp(collected.payloads[2],
                 "SUBMIT|3|4:echo|1|1:y|0|0") == 0,
          "torn: replacement record is readable");
}

static void test_midfile_corruption(void)
{
    remove_wal_files(g_base);

    setenv("IDR_SEGMENT_BYTES", "4096", 1);

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_base))
    {
        CHECK(0, "corruption: open");
        unsetenv("IDR_SEGMENT_BYTES");
        return;
    }

    if (!append_submit(&wal, 1, 200))
    {
        CHECK(0, "corruption: append");
        wal_close(&wal);
        unsetenv("IDR_SEGMENT_BYTES");
        return;
    }

    wal_close(&wal);

    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s.seg.*", g_base);

    glob_t matches;
    memset(&matches, 0, sizeof(matches));

    if (glob(pattern, 0, NULL, &matches) != 0 ||
        matches.gl_pathc < 2)
    {
        CHECK(0, "corruption: at least two segments exist");
        globfree(&matches);
        unsetenv("IDR_SEGMENT_BYTES");
        return;
    }

    const char *first = matches.gl_pathv[0];

    int fd = open(first, O_RDWR);

    if (fd < 0)
    {
        CHECK(0, "corruption: open first segment");
        globfree(&matches);
        unsetenv("IDR_SEGMENT_BYTES");
        return;
    }

    off_t offset = WAL_SEGMENT_HEADER_SIZE + WAL_RECORD_HEADER_SIZE + 10;
    unsigned char byte = 0;

    if (pread(fd, &byte, 1, offset) != 1)
    {
        CHECK(0, "corruption: read payload");
    }
    else
    {
        byte ^= 0xffu;

        if (pwrite(fd, &byte, 1, offset) != 1)
        {
            CHECK(0, "corruption: write payload");
        }
        else
        {
            int scanned = wal_foreach_record(g_base,
                                             0,
                                             collect_cb,
                                             NULL);

            CHECK(scanned == -1,
                  "corruption: mid-file CRC mismatch fails recovery");
        }
    }

    close(fd);
    globfree(&matches);
    unsetenv("IDR_SEGMENT_BYTES");
}

static void test_rejections(void)
{
    remove_wal_files(g_base);

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_base))
    {
        CHECK(0, "reject: open");
        return;
    }

    CHECK(wal_append(&wal, "SUBMIT|1|4:echo|1|1:x|0\n0") == 0,
          "reject: embedded newline");
    CHECK(wal_append(&wal, "BOGUS|1|2|3") == 0,
          "reject: unknown record prefix");
    CHECK(wal_append_typed(&wal,
                           WAL_RECORD_SUBMIT,
                           "SUBMIT|1|4:echo|1|1:x|0|0") == 1,
          "reject: explicit type accepted");

    wal_close(&wal);
}

static void test_stop_early(void)
{
    remove_wal_files(g_base);

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_base))
    {
        CHECK(0, "stop: open");
        return;
    }

    append_submit(&wal, 1, 3);
    wal_close(&wal);

    int scanned = wal_foreach_record(g_base,
                                     0,
                                     stop_early_cb,
                                     NULL);

    CHECK(scanned == 1, "stop: iteration stops after the first record");
}

static void test_missing_and_bad_header(void)
{
    remove_wal_files(g_base);

    uint64_t sequence = 0;

    CHECK(wal_find_active_segment(g_base, &sequence) == 0,
          "missing: no active segment");
    CHECK(wal_foreach_record(g_base, 0, collect_cb, NULL) == 0,
          "missing: no records to replay");
    CHECK(wal_foreach_line(g_base, NULL, NULL) == -1,
          "missing: legacy reader rejects a NULL callback");

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_base))
    {
        CHECK(0, "header: open");
        return;
    }

    wal_close(&wal);

    char path[600];
    if (!wal_segment_path(g_base, 1, path, sizeof(path)))
    {
        CHECK(0, "header: path");
        return;
    }

    int fd = open(path, O_RDWR);

    if (fd < 0)
    {
        CHECK(0, "header: open segment");
        return;
    }

    unsigned char bad = 'X';

    if (pwrite(fd, &bad, 1, 0) != 1)
    {
        CHECK(0, "header: corrupt magic");
    }

    close(fd);

    wal.fd = -1;

    CHECK(wal_open_for_append(&wal, g_base) == 0,
          "header: invalid segment magic is rejected");

    if (wal.fd >= 0)
    {
        wal_close(&wal);
    }
}

static void test_unknown_version(void)
{
    remove_wal_files(g_base);

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, g_base))
    {
        CHECK(0, "version: open");
        return;
    }

    wal_close(&wal);

    char path[600];

    if (!wal_segment_path(g_base, 1, path, sizeof(path)))
    {
        CHECK(0, "version: path");
        return;
    }

    int fd = open(path, O_RDWR);

    if (fd < 0)
    {
        CHECK(0, "version: open segment");
        return;
    }

    unsigned char unknown_version[2] = { 0xe7, 0x03 }; /* 999 */

    if (pwrite(fd, unknown_version, sizeof(unknown_version), 4) !=
        (ssize_t)sizeof(unknown_version))
    {
        CHECK(0, "version: write unknown version");
    }

    close(fd);

    wal.fd = -1;

    CHECK(wal_open_for_append(&wal, g_base) == 0,
          "unknown segment format version is rejected");

    if (wal.fd >= 0)
    {
        wal_close(&wal);
    }

    CHECK(wal_foreach_record(g_base, 0, collect_cb, NULL) == -1,
          "unknown segment format version fails replay");
}

int main(void)
{
    snprintf(g_base,
             sizeof(g_base),
             "/tmp/idr_wal_v2_%d.log",
             (int)getpid());

    setenv("IDR_MIN_FREE_BYTES", "0", 1);

    test_roundtrip_and_filter();
    test_rotation_and_cleanup();
    test_torn_tail();
    test_midfile_corruption();
    test_rejections();
    test_stop_early();
    test_missing_and_bad_header();
    test_unknown_version();

    remove_wal_files(g_base);

    printf("\n%d passed, %d failed\n", passed, failed);

    return failed == 0 ? 0 : 1;
}
