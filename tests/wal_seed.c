/*
 * Test helper: seed a WAL v2 base with framed records.
 *
 *   wal_seed <base> <pairs> [pending] [segment_bytes]
 *
 * Writes <pairs> SUBMIT+RESULT pairs (ids 1..pairs) followed by
 * <pending> SUBMIT-only records (no result). Compiled with
 * IDR_TEST_WAL_NO_SYNC so seeding millions of records is fast;
 * the coordinator under test replays them normally.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wal.h"

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr,
                "usage: %s <base> <pairs> [pending] [segment_bytes]\n",
                argv[0]);
        return 2;
    }

    const char *base = argv[1];
    long pairs = atol(argv[2]);
    long pending = argc > 3 ? atol(argv[3]) : 0;

    setenv("IDR_MIN_FREE_BYTES", "0", 1);

    if (argc > 4 && argv[4][0] != '\0')
    {
        setenv("IDR_SEGMENT_BYTES", argv[4], 1);
    }

    struct Wal wal = { .fd = -1 };

    if (!wal_open_for_append(&wal, base))
    {
        fprintf(stderr, "wal_seed: could not open %s\n", base);
        return 1;
    }

    char line[128];

    for (long i = 1; i <= pairs; i++)
    {
        snprintf(line, sizeof(line),
                 "SUBMIT|%ld|4:echo|1|1:x|0|0", i);

        if (!wal_append(&wal, line))
        {
            fprintf(stderr, "wal_seed: submit %ld failed\n", i);
            return 1;
        }

        snprintf(line, sizeof(line),
                 "RESULT|%ld|1|0|2", i);

        if (!wal_append(&wal, line))
        {
            fprintf(stderr, "wal_seed: result %ld failed\n", i);
            return 1;
        }
    }

    for (long i = 1; i <= pending; i++)
    {
        long id = pairs + i;

        snprintf(line, sizeof(line),
                 "SUBMIT|%ld|4:echo|1|1:x|0|0", id);

        if (!wal_append(&wal, line))
        {
            fprintf(stderr, "wal_seed: pending %ld failed\n", id);
            return 1;
        }
    }

    printf("wal_seed: %ld pair(s) + %ld pending record(s), "
           "next_lsn=%llu active_seq=%llu\n",
           pairs,
           pending,
           (unsigned long long)wal.next_lsn,
           (unsigned long long)wal.active_seq);

    wal_close(&wal);

    return 0;
}
