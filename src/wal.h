#ifndef WAL_H
#define WAL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/*
 * WAL v2.
 *
 * The log is a sequence of segment files:
 *
 *   <base>.seg.<20-digit sequence>
 *
 * Each segment starts with a 24-byte header and contains
 * length-prefixed, CRC32C-protected records. The base path is
 * the value of IDR_WAL_PATH. Snapshots and the manifest live
 * next to the segments:
 *
 *   <base>.snap.<20-digit lsn>.dat
 *   <base>.manifest
 *
 * A legacy single-file WAL at <base> is imported once and
 * renamed to <base>.legacy.
 */

#define WAL_FORMAT_VERSION 2

#define WAL_LINE_MAX 8192

#define WAL_SEGMENT_HEADER_SIZE 24
#define WAL_RECORD_HEADER_SIZE 20
#define WAL_RECORD_OVERHEAD \
    (WAL_RECORD_HEADER_SIZE + 4)

#define WAL_PATH_MAX 1024

#define WAL_DEFAULT_SEGMENT_BYTES (4u * 1024u * 1024u)
#define WAL_DEFAULT_SNAPSHOT_EVERY 10000u
#define WAL_DEFAULT_SNAPSHOT_SECONDS 300u

enum WalRecordType
{
    WAL_RECORD_SUBMIT = 1,
    WAL_RECORD_DISPATCH = 2,
    WAL_RECORD_ACCEPT = 3,
    WAL_RECORD_RESULT = 4
};

struct Wal
{
    int fd;

    char base_path[WAL_PATH_MAX];

    uint64_t active_seq;

    /*
     * LSN to assign to the next record. LSNs start at 1 and
     * never repeat, even across segment truncation.
     */
    uint64_t next_lsn;

    uint64_t segment_bytes;
    uint64_t segment_max_bytes;

    uint64_t records_since_snapshot;
    uint64_t total_records;

    time_t last_snapshot_time;
};

/*
 * Open the active segment for append, creating the first
 * segment if necessary. A torn final record is truncated to the
 * last valid record. Corruption before the final record fails
 * the open.
 *
 * Returns 1 on success, 0 on failure.
 */
int wal_open_for_append(struct Wal *wal, const char *base_path);

int wal_close(struct Wal *wal);

/*
 * Append one record. The caller passes an existing text payload
 * such as "SUBMIT|..." and the record type is inferred from
 * its prefix. The record is fsynced before this returns 1.
 */
int wal_append(struct Wal *wal, const char *line);

/*
 * Append a record with an explicit type. The payload must not
 * contain a newline and must fit in WAL_LINE_MAX.
 */
int wal_append_typed(struct Wal *wal,
                     enum WalRecordType type,
                     const char *payload);

/*
 * Rotate to a new segment. The new segment is created and
 * fsynced before the old descriptor is closed, so a failed
 * rotation leaves the old WAL usable.
 */
int wal_rotate(struct Wal *wal);

/*
 * Return 1 when a snapshot should be taken now.
 */
int wal_should_snapshot(const struct Wal *wal);

/*
 * Record callback. Return 0 to stop iteration, 1 to continue.
 */
typedef int (*wal_record_cb)(enum WalRecordType type,
                             uint64_t lsn,
                             const char *payload,
                             void *user);

/*
 * Read every segment in order. Records with lsn <= after_lsn
 * are validated but not delivered.
 *
 * A torn final record in the newest segment is truncated to
 * the last valid record and reported with a warning. A torn or
 * corrupt record before the newest segment fails recovery.
 *
 * Returns the number of records scanned (including skipped
 * ones), or -1 on corruption/read error.
 */
int wal_foreach_record(const char *base_path,
                       uint64_t after_lsn,
                       wal_record_cb cb,
                       void *user);

/*
 * Legacy text reader for pre-v2 <base> files.
 */
typedef int (*wal_line_cb)(const char *line, void *user);

int wal_foreach_line(const char *path,
                     wal_line_cb cb,
                     void *user);

/*
 * Path helpers. Return 1 on success, 0 when the output buffer
 * is too small.
 */
int wal_segment_path(const char *base_path,
                     uint64_t sequence,
                     char *out,
                     size_t out_size);

int wal_snapshot_path(const char *base_path,
                      uint64_t lsn,
                      char *out,
                      size_t out_size);

int wal_manifest_path(const char *base_path,
                      char *out,
                      size_t out_size);

/*
 * Find the highest-numbered segment. Returns 1 when found
 * (sequence written to *sequence), 0 when no segments exist,
 * and -1 on error.
 */
int wal_find_active_segment(const char *base_path,
                            uint64_t *sequence);

/*
 * Delete segment files with sequence < keep_from.
 * Returns the number removed, or -1 on error.
 */
int wal_remove_segments_before(const char *base_path,
                               uint64_t keep_from);

/*
 * Fsync the directory containing path.
 */
int wal_fsync_directory(const char *path);

uint32_t wal_crc32c(uint32_t seed, const void *data, size_t length);

int wal_parse_record_type(const char *payload,
                          enum WalRecordType *type);

/*
 * Test-only crash injection. If IDR_TEST_CRASH_AT equals name,
 * the process exits immediately with status 99.
 */
void idr_test_crash_point(const char *name);

#endif
