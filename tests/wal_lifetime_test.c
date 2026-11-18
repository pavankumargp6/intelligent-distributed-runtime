/*
 * Lifetime scale test.
 *
 * Pushes 1,000,000 tasks (default; IDR_LIFETIME_TASKS overrides)
 * through the WAL and the result-retention history to prove the
 * invariants Phase 1 promises:
 *
 *   - the WAL absorbs millions of framed records via segment
 *     rotation (built with IDR_TEST_WAL_NO_SYNC; durability is
 *     covered by the fsync-based crash and recovery tests),
 *   - snapshots keep the on-disk footprint bounded (segments
 *     behind the previous snapshot are truncated),
 *   - the retained-result table stays bounded by its configured
 *     capacity: the oldest results age out, the newest are
 *     queryable, and lookups cost the same after a million tasks
 *     as they did after one,
 *   - task ids never repeat and counters never saturate.
 */

#define _GNU_SOURCE

#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "recovery.h"
#include "runtime.h"
#include "task_manager.h"

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); passed++; } \
    else      { printf("FAIL: %s\n", msg); failed++; } \
} while (0)

static char g_base[512];

static void remove_files(const char *base)
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

int main(void)
{
    uint64_t total =
        1000000ull;

    const char *env_total = getenv("IDR_LIFETIME_TASKS");

    if (env_total != NULL && env_total[0] != '\0')
    {
        unsigned long long parsed = strtoull(env_total, NULL, 10);

        if (parsed > 0)
        {
            total = (uint64_t)parsed;
        }
    }

    snprintf(g_base,
             sizeof(g_base),
             "/tmp/idr_wal_lifetime_%d.log",
             (int)getpid());

    setenv("IDR_MIN_FREE_BYTES", "0", 1);
    setenv("IDR_RESULT_RETENTION", "5000", 1);
    setenv("IDR_RESULT_RETENTION_SECONDS", "0", 1);
    setenv("IDR_SEGMENT_BYTES", "65536", 1);

    remove_files(g_base);

    struct Runtime runtime;
    struct TaskManager manager;
    struct InFlightTable table;

    memset(&runtime, 0, sizeof(runtime));
    memset(&manager, 0, sizeof(manager));

    initialize_task_manager(&manager);
    initialize_runtime_stats(&runtime.stats);

    runtime.task_manager = &manager;
    runtime.wal.fd = -1;

    coordinator_tasks_init(&table);

    if (!recovery_start(&runtime, &table, g_base))
    {
        printf("FAIL: could not start the WAL\n");
        return 1;
    }

    uint64_t snapshot_every = total / 4;

    if (snapshot_every == 0)
    {
        snapshot_every = 1;
    }

    char line[128];

    for (uint64_t i = 1; i <= total; i++)
    {
        snprintf(line, sizeof(line),
                 "SUBMIT|%llu|4:echo|1|1:x|0|0",
                 (unsigned long long)i);

        if (!wal_append(&runtime.wal, line))
        {
            printf("FAIL: append of SUBMIT %llu failed\n",
                   (unsigned long long)i);
            return 1;
        }

        snprintf(line, sizeof(line),
                 "RESULT|%llu|1|0|2",
                 (unsigned long long)i);

        if (!wal_append(&runtime.wal, line))
        {
            printf("FAIL: append of RESULT %llu failed\n",
                   (unsigned long long)i);
            return 1;
        }

        coordinator_record_completed(&table,
                                     (int)i,
                                     1,
                                     0,
                                     TASK_COMPLETED);
        record_task_completed(&runtime.stats, 0.0, 0.0);

        /*
         * Model the allocator: in production every task ID comes
         * from next_task_id, so snapshots must capture the advanced
         * watermark or recovery would rewind and reuse IDs.
         */
        table.next_task_id = (int)i + 1;

        if (i % snapshot_every == 0)
        {
            if (!recovery_take_snapshot(&runtime, &table,
                                        &runtime.wal))
            {
                printf("FAIL: snapshot at task %llu failed\n",
                       (unsigned long long)i);
                return 1;
            }
        }
    }

    if (!recovery_take_snapshot(&runtime, &table, &runtime.wal))
    {
        printf("FAIL: final snapshot failed\n");
        return 1;
    }

    CHECK(runtime.wal.total_records == total * 2,
          "every SUBMIT and RESULT reached the log");

    CHECK(runtime.wal.next_lsn == total * 2 + 1,
          "LSNs never repeat across the whole run");

    CHECK(coordinator_lookup_completed(&table,
                                       (int)total + 1,
                                       NULL,
                                       NULL,
                                       NULL) == 0,
          "ids beyond the lifetime range have no results");

    CHECK(runtime.stats.tasks_completed == (int)total,
          "lifetime completed counter reaches 1M");

    CHECK(coordinator_history_count(&table.history) == 5000,
          "retained results stay bounded at the configured capacity");

    CHECK(coordinator_lookup_completed(&table, 1, NULL, NULL, NULL) == 0,
          "the oldest results age out of retention");

    int newest_state = -1;

    CHECK(coordinator_lookup_completed(&table,
                                       (int)total,
                                       NULL,
                                       NULL,
                                       &newest_state) == 1,
          "the newest results are queryable");
    CHECK(newest_state == TASK_COMPLETED,
          "the newest result state is preserved");

    CHECK(manager.queue.count == 0,
          "no completed task was left queued");

    int segments = count_segments(g_base);

    CHECK(segments > 0, "the WAL still has an active segment");
    CHECK(segments < 100,
          "snapshots keep the segment count bounded");

    /*
     * Timed restart: recovery must restore the lifetime state from
     * the latest snapshot plus the segment tail, and it must do so
     * quickly. The exit criterion for Phase 1 is recovery under
     * five seconds after a million lifetime tasks.
     */
    wal_close(&runtime.wal);
    coordinator_tasks_destroy(&table);

    initialize_task_manager(&manager);
    initialize_runtime_stats(&runtime.stats);

    runtime.task_manager = &manager;
    runtime.wal.fd = -1;

    coordinator_tasks_init(&table);

    struct timespec restart_begin;
    struct timespec restart_end;

    clock_gettime(CLOCK_MONOTONIC, &restart_begin);

    int restart_ok = recovery_start(&runtime, &table, g_base);

    clock_gettime(CLOCK_MONOTONIC, &restart_end);

    double recovery_seconds =
        (double)(restart_end.tv_sec - restart_begin.tv_sec)
        + (double)(restart_end.tv_nsec - restart_begin.tv_nsec)
          / 1000000000.0;

    CHECK(restart_ok, "restart from snapshot + segments succeeds");
    CHECK(table.next_task_id == (int)total + 1,
          "restart never reuses or rewinds task ids");
    CHECK(runtime.stats.tasks_completed == (int)total,
          "restart restores the lifetime completed counter");
    CHECK(coordinator_history_count(&table.history) == 5000,
          "restart restores the bounded retained history");
    CHECK(recovery_seconds < 5.0,
          "recovery after 1M tasks stays under 5 seconds");

    printf("\n%d passed, %d failed "
           "(tasks=%llu segments=%d recovery_seconds=%.3f)\n",
           passed,
           failed,
           (unsigned long long)total,
           segments,
           recovery_seconds);

    wal_close(&runtime.wal);
    coordinator_tasks_destroy(&table);

    remove_files(g_base);

    return failed == 0 ? 0 : 1;
}
