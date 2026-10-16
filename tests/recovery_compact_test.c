/*
 * Unit test for recovery_compact_wal.
 *
 * Compaction should:
 *   - succeed on a missing file (nothing to do)
 *   - succeed on an empty file (nothing to do)
 *   - keep RESULT records for completed tasks
 *   - keep SUBMIT records for every task, including completed ones
 *   - be idempotent: a second pass changes nothing
 *
 * History note: an earlier policy dropped the SUBMIT once a
 * matching RESULT existed. That was a correctness bug — the
 * SUBMIT carries command, arguments, idempotent, and
 * timeout_seconds, and recovery_replay_wal iterates over SUBMITs
 * matching each against a RESULT. Dropping the SUBMIT left the
 * RESULT orphaned and the task silently lost on restart. This
 * test now pins the corrected behavior.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "recovery.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS: %s\n", msg); } \
    else { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static int count_lines(const char *path)
{
    FILE *f = fopen(path, "r");

    if (f == NULL)
    {
        return -1;
    }

    int n = 0;
    char line[8192];

    while (fgets(line, sizeof(line), f) != NULL)
    {
        n++;
    }

    fclose(f);
    return n;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");

    if (f == NULL)
    {
        return 0;
    }

    char line[8192];

    while (fgets(line, sizeof(line), f) != NULL)
    {
        size_t len = strlen(line);

        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
            line[--len] = '\0';
        }

        if (strcmp(line, needle) == 0)
        {
            fclose(f);
            return 1;
        }
    }

    fclose(f);
    return 0;
}

int main(void)
{
    char path[256];
    snprintf(path,
             sizeof(path),
             "/tmp/idr_recovery_compact_%d.log",
             (int)getpid());

    unlink(path);

    /* ---- Case 1: missing file ---- */
    CHECK(recovery_compact_wal(path) == 1,
          "compact on missing file returns 1");

    /* ---- Case 2: empty file ---- */
    FILE *f = fopen(path, "w");

    if (f == NULL)
    {
        printf("FAIL: could not create test file\n");
        return 1;
    }
    fclose(f);

    CHECK(recovery_compact_wal(path) == 1,
          "compact on empty file returns 1");
    CHECK(count_lines(path) == 0,
          "empty file still empty after compact");

    /* ---- Case 3: mixed records ---- */
    f = fopen(path, "w");

    if (f == NULL)
    {
        printf("FAIL: could not create test file\n");
        return 1;
    }

    fprintf(f, "SUBMIT|1|4:echo|1|4:task|0\n");
    fprintf(f, "RESULT|1|1|0|2\n");
    fprintf(f, "SUBMIT|2|5:sleep|1|2:30|0\n");
    fprintf(f, "SUBMIT|3|4:echo|1|4:task|0\n");
    fprintf(f, "RESULT|3|1|0|2\n");
    fclose(f);

    CHECK(recovery_compact_wal(path) == 1,
          "compact on mixed file returns 1");

    /*
     * Every SUBMIT and RESULT line is preserved. No records are
     * dropped, so the compacted file has the same number of lines
     * as the input.
     */
    CHECK(count_lines(path) == 5,
          "compacted file preserves all 5 lines");

    CHECK(file_contains(path, "SUBMIT|1|4:echo|1|4:task|0"),
          "SUBMIT for completed task 1 kept");
    CHECK(file_contains(path, "RESULT|1|1|0|2"),
          "RESULT for completed task 1 kept");
    CHECK(file_contains(path, "SUBMIT|2|5:sleep|1|2:30|0"),
          "SUBMIT for pending task 2 kept");
    CHECK(file_contains(path, "SUBMIT|3|4:echo|1|4:task|0"),
          "SUBMIT for completed task 3 kept");
    CHECK(file_contains(path, "RESULT|3|1|0|2"),
          "RESULT for completed task 3 kept");

    /* ---- Case 4: idempotent compaction ---- */
    CHECK(recovery_compact_wal(path) == 1,
          "compact on already-compacted file returns 1");
    CHECK(count_lines(path) == 5,
          "second compaction does not change line count");

    unlink(path);

    if (failures == 0)
    {
        printf("\nrecovery_compact_test: PASS\n");
        return 0;
    }

    printf("\nrecovery_compact_test: FAIL (%d)\n", failures);
    return 1;
}
