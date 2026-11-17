/*
 * Snapshot + recovery integration test.
 *
 * Exercises the WAL v2 recovery path without sockets:
 *
 *   1. Fresh start creates a segment, manifest, and startup snapshot.
 *   2. A second snapshot captures a queued task and an in-flight
 *      non-idempotent task.
 *   3. Recovery replays the snapshot: the queued task is restored,
 *      and the abandoned non-idempotent task becomes TASK_UNKNOWN.
 *   4. If the newest snapshot is corrupted, recovery falls back to
 *      the previous snapshot and replays the segments after it.
 *   5. A legacy text WAL is imported once and renamed.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static int count_snapshots(const char *base)
{
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s.snap.*", base);

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

static int newest_snapshot(const char *base, char *out, size_t out_size)
{
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s.snap.*", base);

    glob_t matches;
    memset(&matches, 0, sizeof(matches));

    if (glob(pattern, 0, NULL, &matches) != 0 ||
        matches.gl_pathc == 0)
    {
        globfree(&matches);
        return 0;
    }

    size_t best = 0;

    for (size_t i = 1; i < matches.gl_pathc; i++)
    {
        if (strcmp(matches.gl_pathv[i],
                   matches.gl_pathv[best]) > 0)
        {
            best = i;
        }
    }

    snprintf(out, out_size, "%s", matches.gl_pathv[best]);
    globfree(&matches);

    return 1;
}

static void init_harness(struct Runtime *runtime,
                         struct TaskManager *manager,
                         struct InFlightTable *table)
{
    memset(runtime, 0, sizeof(*runtime));
    memset(manager, 0, sizeof(*manager));

    initialize_task_manager(manager);
    initialize_runtime_stats(&runtime->stats);

    runtime->task_manager = manager;
    runtime->wal.fd = -1;

    coordinator_tasks_init(table);
}

static struct Task make_task(int id, const char *command, int idempotent)
{
    struct Task task;
    memset(&task, 0, sizeof(task));

    task.id = id;
    task.assigned_worker_id = -1;
    task.idempotent = idempotent;
    task.state = TASK_CREATED;
    snprintf(task.command, sizeof(task.command), "%s", command);

    return task;
}

static void test_snapshot_roundtrip(void)
{
    remove_files(g_base);

    struct Runtime runtime;
    struct TaskManager manager;
    struct InFlightTable table;

    init_harness(&runtime, &manager, &table);

    CHECK(recovery_start(&runtime, &table, g_base) == 1,
          "fresh start succeeds");
    CHECK(runtime.wal.fd >= 0, "fresh start leaves the WAL open");
    CHECK(runtime.wal.next_lsn == 1,
          "fresh start begins at LSN 1");
    CHECK(count_snapshots(g_base) == 1,
          "fresh start writes one startup snapshot");
    CHECK(table.next_task_id == 1,
          "fresh start begins at task id 1");

    /*
     * Task 1 waits in the queue. Task 2 is in flight on worker 2
     * and is not idempotent, so recovery must mark it unknown.
     */
    struct Task queued = make_task(1, "echo", 1);
    struct Task inflight = make_task(2, "charge-card", 0);

    runtime.stats.tasks_submitted = 2;
    table.next_task_id = 3;

    CHECK(submit_task(&manager, &queued) == 1,
          "queued task accepted by the manager");
    CHECK(coordinator_record_inflight(&table, &inflight, 2) == 1,
          "in-flight task recorded");

    CHECK(wal_append(&runtime.wal,
                     "SUBMIT|1|4:echo|1|0|1|0") == 1,
          "queued SUBMIT appended");
    CHECK(wal_append(&runtime.wal,
                     "SUBMIT|2|11:charge-card|1|0|0|0") == 1,
          "in-flight SUBMIT appended");
    CHECK(wal_append(&runtime.wal,
                     "DISPATCH|2|2|0|1|1700000000") == 1,
          "DISPATCH appended");

    CHECK(recovery_take_snapshot(&runtime, &table, &runtime.wal) == 1,
          "explicit snapshot succeeds");
    CHECK(count_snapshots(g_base) == 2,
          "explicit snapshot adds a second snapshot file");
    CHECK(runtime.wal.active_seq >= 2,
          "snapshot rotated to a new segment");

    wal_close(&runtime.wal);
    coordinator_tasks_destroy(&table);
}

static void test_recovery_policy(void)
{
    struct Runtime runtime;
    struct TaskManager manager;
    struct InFlightTable table;

    init_harness(&runtime, &manager, &table);

    CHECK(recovery_start(&runtime, &table, g_base) == 1,
          "recovery after snapshot succeeds");
    CHECK(table.next_task_id == 3,
          "recovery restores next_task_id");
    CHECK(runtime.stats.tasks_submitted == 2,
          "recovery restores submitted count");
    CHECK(manager.queue.count == 1,
          "queued task restored");
    CHECK(coordinator_is_inflight(&table, 2) == 0,
          "abandoned task removed from in-flight");

    int worker = -1;
    int exit_status = -1;
    int state = -1;

    CHECK(coordinator_lookup_completed(&table,
                                       2,
                                       &worker,
                                       &exit_status,
                                       &state) == 1,
          "abandoned non-idempotent task has a terminal record");
    CHECK(worker == 2, "terminal record keeps the dispatched worker");
    CHECK(exit_status == -1, "terminal record has no exit status");
    CHECK(state == TASK_UNKNOWN,
          "abandoned non-idempotent task is TASK_UNKNOWN");
    CHECK(runtime.stats.tasks_unknown == 1,
          "unknown counter incremented");
    CHECK(runtime.stats.tasks_failed == 0,
          "unknown is not counted as failed");

    char wal_line[128];
    snprintf(wal_line, sizeof(wal_line), "RESULT|2|2|-1|5");

    CHECK(wal_append(&runtime.wal, wal_line) == 1,
          "WAL remains appendable after recovery");

    CHECK(recovery_take_snapshot(&runtime, &table, &runtime.wal) == 1,
          "recovered state can be snapshotted again");

    wal_close(&runtime.wal);
    coordinator_tasks_destroy(&table);
}

static void test_snapshot_fallback(void)
{
    char snapshot[600];

    if (!newest_snapshot(g_base, snapshot, sizeof(snapshot)))
    {
        CHECK(0, "fallback: newest snapshot exists");
        return;
    }

    int fd = open(snapshot, O_RDWR);

    if (fd < 0)
    {
        CHECK(0, "fallback: open newest snapshot");
        return;
    }

    struct stat info;

    if (fstat(fd, &info) != 0 || info.st_size < 200)
    {
        CHECK(0, "fallback: snapshot is large enough");
        close(fd);
        return;
    }

    unsigned char byte = 0;
    off_t offset = info.st_size - 64;

    if (pread(fd, &byte, 1, offset) != 1 ||
        pwrite(fd, &byte, 1, offset) != 1)
    {
        CHECK(0, "fallback: corrupt snapshot");
        close(fd);
        return;
    }

    close(fd);

    struct Runtime runtime;
    struct TaskManager manager;
    struct InFlightTable table;

    init_harness(&runtime, &manager, &table);

    CHECK(recovery_start(&runtime, &table, g_base) == 1,
          "recovery falls back to the previous snapshot");
    CHECK(table.next_task_id == 3,
          "fallback keeps next_task_id");
    CHECK(manager.queue.count == 1,
          "fallback replays the queued task");

    int state = -1;

    CHECK(coordinator_lookup_completed(&table,
                                       2,
                                       NULL,
                                       NULL,
                                       &state) == 1,
          "fallback replays the RESULT after the previous snapshot");
    CHECK(state == TASK_UNKNOWN,
          "fallback restores TASK_UNKNOWN for the abandoned task");

    wal_close(&runtime.wal);
    coordinator_tasks_destroy(&table);
}

static void test_legacy_import(void)
{
    remove_files(g_base);

    FILE *file = fopen(g_base, "w");

    if (file == NULL)
    {
        CHECK(0, "legacy: create text WAL");
        return;
    }

    fprintf(file, "SUBMIT|1|4:echo|1|3:one|0|0\n");
    fprintf(file, "RESULT|1|1|0|2\n");
    fprintf(file, "SUBMIT|2|4:echo|1|3:two|0|0\n");
    fclose(file);

    struct Runtime runtime;
    struct TaskManager manager;
    struct InFlightTable table;

    init_harness(&runtime, &manager, &table);

    CHECK(recovery_start(&runtime, &table, g_base) == 1,
          "legacy import succeeds");
    CHECK(access(g_base, F_OK) != 0,
          "legacy file is moved out of the active WAL path");

    char legacy[600];
    snprintf(legacy, sizeof(legacy), "%s.legacy", g_base);
    CHECK(access(legacy, F_OK) == 0,
          "legacy file is preserved as .legacy");
    CHECK(manager.queue.count == 1,
          "legacy pending task is queued");
    CHECK(coordinator_lookup_completed(&table,
                                       1,
                                       NULL,
                                       NULL,
                                       NULL) == 1,
          "legacy completed result is retained");
    CHECK(table.next_task_id == 3,
          "legacy task ids are not reused");

    wal_close(&runtime.wal);
    coordinator_tasks_destroy(&table);
}

int main(void)
{
    snprintf(g_base,
             sizeof(g_base),
             "/tmp/idr_recovery_snapshot_%d.log",
             (int)getpid());

    setenv("IDR_MIN_FREE_BYTES", "0", 1);
    setenv("IDR_RESULT_RETENTION", "1000", 1);
    setenv("IDR_SNAPSHOT_EVERY", "1000000", 1);

    test_snapshot_roundtrip();
    test_recovery_policy();
    test_snapshot_fallback();
    test_legacy_import();

    remove_files(g_base);

    printf("\n%d passed, %d failed\n", passed, failed);

    return failed == 0 ? 0 : 1;
}
