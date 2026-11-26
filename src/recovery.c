#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "recovery.h"
#include "wal.h"
#include "wire_fields.h"
#include "log.h"
#include "task_queue.h"

#define SNAPSHOT_MAGIC "IDRSNP2"
#define SNAPSHOT_VERSION 2
#define SNAPSHOT_HEADER_SIZE 140
#define SNAPSHOT_MAX_BYTES (512u * 1024u * 1024u)

#define SNAPSHOT_MAX_QUEUED 1000000u
#define SNAPSHOT_MAX_INFLIGHT 1000000u
#define SNAPSHOT_MAX_HISTORY 10000000u

struct SnapshotMeta
{
    uint64_t lsn;
    uint64_t boundary_seq;
    uint64_t created;
    int valid;
};

struct Manifest
{
    struct SnapshotMeta current;
    struct SnapshotMeta previous;
};

struct SnapshotInflight
{
    struct Task task;
    int worker_id;
    time_t dispatched_at;
    int accepted;
};

struct SnapshotData
{
    int valid;

    uint64_t lsn;
    uint64_t boundary_seq;
    uint64_t next_task_id;
    uint64_t created;

    struct RuntimeStats stats;

    struct Task *queued;
    size_t queued_count;

    struct SnapshotInflight *inflight;
    size_t inflight_count;

    struct HistoryRecord *history;
    size_t history_count;
};

struct Buffer
{
    unsigned char *data;
    size_t length;
    size_t capacity;
    int failed;
};

struct Reader
{
    const unsigned char *data;
    size_t length;
    size_t offset;
    int failed;
};

static void put_u16_le(unsigned char *out, uint16_t value)
{
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8) & 0xffu);
}

static void put_u32_le(unsigned char *out, uint32_t value)
{
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8) & 0xffu);
    out[2] = (unsigned char)((value >> 16) & 0xffu);
    out[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void put_u64_le(unsigned char *out, uint64_t value)
{
    for (int i = 0; i < 8; i++)
    {
        out[i] = (unsigned char)((value >> (8 * i)) & 0xffu);
    }
}

static uint16_t get_u16_le(const unsigned char *in)
{
    return (uint16_t)in[0] |
           (uint16_t)((uint16_t)in[1] << 8);
}

static uint32_t get_u32_le(const unsigned char *in)
{
    return (uint32_t)in[0] |
           ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static uint64_t get_u64_le(const unsigned char *in)
{
    uint64_t value = 0;

    for (int i = 0; i < 8; i++)
    {
        value |= (uint64_t)in[i] << (8 * i);
    }

    return value;
}

static int buffer_reserve(struct Buffer *buffer, size_t extra)
{
    if (buffer->failed)
    {
        return 0;
    }

    if (extra > SIZE_MAX - buffer->length)
    {
        buffer->failed = 1;
        return 0;
    }

    size_t needed = buffer->length + extra;

    if (needed <= buffer->capacity)
    {
        return 1;
    }

    size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;

    while (capacity < needed)
    {
        if (capacity > SIZE_MAX / 2)
        {
            capacity = needed;
            break;
        }

        capacity *= 2;
    }

    unsigned char *data = realloc(buffer->data, capacity);

    if (data == NULL)
    {
        buffer->failed = 1;
        return 0;
    }

    buffer->data = data;
    buffer->capacity = capacity;

    return 1;
}

static void buffer_put(struct Buffer *buffer,
                       const void *data,
                       size_t length)
{
    if (!buffer_reserve(buffer, length))
    {
        return;
    }

    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
}

static void buffer_u8(struct Buffer *buffer, uint8_t value)
{
    buffer_put(buffer, &value, 1);
}

static void buffer_u32(struct Buffer *buffer, uint32_t value)
{
    unsigned char bytes[4];
    put_u32_le(bytes, value);
    buffer_put(buffer, bytes, sizeof(bytes));
}

static void buffer_u64(struct Buffer *buffer, uint64_t value)
{
    unsigned char bytes[8];
    put_u64_le(bytes, value);
    buffer_put(buffer, bytes, sizeof(bytes));
}

static void buffer_i64(struct Buffer *buffer, int64_t value)
{
    buffer_u64(buffer, (uint64_t)value);
}

static void buffer_f64(struct Buffer *buffer, double value)
{
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    buffer_u64(buffer, bits);
}

static void buffer_string(struct Buffer *buffer, const char *value)
{
    size_t length = value == NULL ? 0 : strlen(value);

    if (length > UINT32_MAX)
    {
        buffer->failed = 1;
        return;
    }

    buffer_u32(buffer, (uint32_t)length);
    buffer_put(buffer, value, length);
}

static void buffer_free(struct Buffer *buffer)
{
    if (buffer == NULL)
    {
        return;
    }

    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static uint8_t reader_u8(struct Reader *reader)
{
    if (reader->failed || reader->offset + 1 > reader->length)
    {
        reader->failed = 1;
        return 0;
    }

    return reader->data[reader->offset++];
}

static uint32_t reader_u32(struct Reader *reader)
{
    if (reader->failed || reader->offset + 4 > reader->length)
    {
        reader->failed = 1;
        return 0;
    }

    uint32_t value = get_u32_le(reader->data + reader->offset);
    reader->offset += 4;
    return value;
}

static uint64_t reader_u64(struct Reader *reader)
{
    if (reader->failed || reader->offset + 8 > reader->length)
    {
        reader->failed = 1;
        return 0;
    }

    uint64_t value = get_u64_le(reader->data + reader->offset);
    reader->offset += 8;
    return value;
}

static int64_t reader_i64(struct Reader *reader)
{
    return (int64_t)reader_u64(reader);
}

static double reader_f64(struct Reader *reader)
{
    uint64_t bits = reader_u64(reader);
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static int reader_bytes(struct Reader *reader,
                        void *out,
                        size_t length)
{
    if (reader->failed || reader->offset + length > reader->length)
    {
        reader->failed = 1;
        return 0;
    }

    memcpy(out, reader->data + reader->offset, length);
    reader->offset += length;

    return 1;
}

static int reader_string(struct Reader *reader,
                         char *out,
                         size_t out_size)
{
    uint32_t length = reader_u32(reader);

    if (reader->failed || length >= out_size)
    {
        reader->failed = 1;
        return 0;
    }

    if (!reader_bytes(reader, out, length))
    {
        return 0;
    }

    out[length] = '\0';

    return 1;
}

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t written = 0;

    while (written < length)
    {
        ssize_t n = write(fd, bytes + written, length - written);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return 0;
        }

        if (n == 0)
        {
            return 0;
        }

        written += (size_t)n;
    }

    return 1;
}

static int write_file_atomic(const char *path,
                             const char *tmp_path,
                             const void *data,
                             size_t length)
{
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
    {
        log_error("recovery_write_open_failed",
                  "Could not open %s: %s", tmp_path, strerror(errno));
        return 0;
    }

    if (!write_all(fd, data, length) || fsync(fd) != 0)
    {
        log_error("recovery_write_failed",
                  "Could not write %s: %s", tmp_path, strerror(errno));
        close(fd);
        unlink(tmp_path);
        return 0;
    }

    if (close(fd) != 0)
    {
        unlink(tmp_path);
        return 0;
    }

    if (rename(tmp_path, path) != 0)
    {
        log_error("recovery_rename_failed",
                  "Could not rename %s to %s: %s",
                  tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return 0;
    }

    if (!wal_fsync_directory(path))
    {
        log_warn("recovery_dir_fsync_failed",
                 "Could not fsync directory for %s", path);
    }

    return 1;
}

static int read_entire_file(const char *path,
                            unsigned char **data_out,
                            size_t *size_out)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0)
    {
        return 0;
    }

    struct stat info;

    if (fstat(fd, &info) != 0 ||
        info.st_size < 0 ||
        (uint64_t)info.st_size > SNAPSHOT_MAX_BYTES)
    {
        close(fd);
        return 0;
    }

    size_t size = (size_t)info.st_size;
    unsigned char *data = malloc(size == 0 ? 1 : size);

    if (data == NULL)
    {
        close(fd);
        return 0;
    }

    size_t offset = 0;

    while (offset < size)
    {
        ssize_t n = read(fd, data + offset, size - offset);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            free(data);
            close(fd);
            return 0;
        }

        if (n == 0)
        {
            break;
        }

        offset += (size_t)n;
    }

    close(fd);

    if (offset != size)
    {
        free(data);
        return 0;
    }

    *data_out = data;
    *size_out = size;

    return 1;
}

static int parse_snapshot_lsn(const char *path, uint64_t *lsn)
{
    const char *tag = NULL;

    for (const char *cursor = path;
         (cursor = strstr(cursor, ".snap.")) != NULL;
         cursor += 6)
    {
        tag = cursor;
    }

    if (tag == NULL)
    {
        return 0;
    }

    tag += 6;

    char *end = NULL;
    unsigned long long value = strtoull(tag, &end, 10);

    if (end == tag || end == NULL || strcmp(end, ".dat") != 0)
    {
        return 0;
    }

    *lsn = (uint64_t)value;

    return 1;
}

static int find_latest_snapshot(const char *base_path,
                                uint64_t *lsn_out)
{
    char pattern[WAL_PATH_MAX];

    int n = snprintf(pattern, sizeof(pattern),
                     "%s.snap.*.dat", base_path);

    if (n <= 0 || (size_t)n >= sizeof(pattern))
    {
        return 0;
    }

    glob_t matches;

    memset(&matches, 0, sizeof(matches));

    int rc = glob(pattern, 0, NULL, &matches);

    if (rc == GLOB_NOMATCH)
    {
        globfree(&matches);
        return 0;
    }

    if (rc != 0)
    {
        globfree(&matches);
        return 0;
    }

    int found = 0;
    uint64_t highest = 0;

    for (size_t i = 0; i < matches.gl_pathc; i++)
    {
        uint64_t value = 0;

        if (!parse_snapshot_lsn(matches.gl_pathv[i], &value))
        {
            continue;
        }

        if (!found || value > highest)
        {
            highest = value;
            found = 1;
        }
    }

    globfree(&matches);

    if (found)
    {
        *lsn_out = highest;
    }

    return found;
}

static void snapshot_data_free(struct SnapshotData *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }

    free(snapshot->queued);
    free(snapshot->inflight);
    free(snapshot->history);
    memset(snapshot, 0, sizeof(*snapshot));
}

static void snapshot_write_task(struct Buffer *buffer,
                                const struct Task *task)
{
    buffer_u32(buffer, (uint32_t)task->id);
    buffer_u32(buffer, (uint32_t)task->state);
    buffer_u32(buffer, (uint32_t)task->idempotent);
    buffer_u32(buffer, (uint32_t)task->timeout_seconds);
    buffer_u32(buffer, (uint32_t)task->assigned_worker_id);
    buffer_u32(buffer, (uint32_t)task->attempt);
    buffer_u32(buffer, (uint32_t)task->pid);
    buffer_u32(buffer, (uint32_t)task->exit_status);
    buffer_f64(buffer, task->required_cpu_percent);
    buffer_i64(buffer, (int64_t)task->required_memory_mb);
    buffer_string(buffer, task->command);
    buffer_u32(buffer, (uint32_t)task->argument_count);

    for (int i = 0; i < task->argument_count; i++)
    {
        buffer_string(buffer, task->arguments[i]);
    }
}

static int snapshot_read_task(struct Reader *reader,
                              struct Task *task)
{
    memset(task, 0, sizeof(*task));

    int32_t id = (int32_t)reader_u32(reader);
    int32_t state = (int32_t)reader_u32(reader);
    int32_t idempotent = (int32_t)reader_u32(reader);
    int32_t timeout = (int32_t)reader_u32(reader);
    int32_t worker = (int32_t)reader_u32(reader);
    int32_t attempt = (int32_t)reader_u32(reader);
    int32_t pid = (int32_t)reader_u32(reader);
    int32_t exit_status = (int32_t)reader_u32(reader);

    double cpu = reader_f64(reader);
    int64_t memory = reader_i64(reader);

    if (reader->failed ||
        id <= 0 ||
        state < TASK_CREATED || state > TASK_UNKNOWN ||
        timeout < 0 ||
        attempt < 0)
    {
        return 0;
    }

    task->id = id;
    task->state = (enum TaskState)state;
    task->idempotent = idempotent != 0;
    task->timeout_seconds = timeout;
    task->assigned_worker_id = worker;
    task->attempt = attempt;
    task->pid = pid;
    task->exit_status = exit_status;
    task->required_cpu_percent = cpu;
    task->required_memory_mb = (long)memory;

    if (!reader_string(reader,
                       task->command,
                       sizeof(task->command)))
    {
        return 0;
    }

    uint32_t argument_count = reader_u32(reader);

    if (reader->failed ||
        argument_count > TASK_ARGUMENT_COUNT)
    {
        return 0;
    }

    task->argument_count = (int)argument_count;

    for (uint32_t i = 0; i < argument_count; i++)
    {
        if (!reader_string(reader,
                           task->arguments[i],
                           sizeof(task->arguments[i])))
        {
            return 0;
        }
    }

    return 1;
}

static void snapshot_build_header(unsigned char *header,
                                  const struct SnapshotData *snapshot,
                                  const struct Buffer *body)
{
    memset(header, 0, SNAPSHOT_HEADER_SIZE);

    memcpy(header, SNAPSHOT_MAGIC, 7);

    put_u16_le(header + 8, SNAPSHOT_VERSION);
    put_u16_le(header + 10, SNAPSHOT_HEADER_SIZE);
    put_u64_le(header + 12, snapshot->lsn);
    put_u64_le(header + 20, snapshot->next_task_id);
    put_u64_le(header + 28, snapshot->created);
    put_u64_le(header + 36, snapshot->boundary_seq);
    put_u64_le(header + 44, snapshot->queued_count);
    put_u64_le(header + 52, snapshot->inflight_count);
    put_u64_le(header + 60, snapshot->history_count);
    put_u64_le(header + 68, (uint64_t)(int64_t)snapshot->stats.tasks_submitted);
    put_u64_le(header + 76, (uint64_t)(int64_t)snapshot->stats.tasks_completed);
    put_u64_le(header + 84, (uint64_t)(int64_t)snapshot->stats.tasks_failed);
    put_u64_le(header + 92, (uint64_t)(int64_t)snapshot->stats.tasks_timed_out);
    put_u64_le(header + 100, (uint64_t)(int64_t)snapshot->stats.tasks_unknown);

    uint64_t queue_bits = 0;
    uint64_t exec_bits = 0;

    memcpy(&queue_bits, &snapshot->stats.total_queue_wait_seconds,
           sizeof(queue_bits));
    memcpy(&exec_bits, &snapshot->stats.total_execution_seconds,
           sizeof(exec_bits));

    put_u64_le(header + 108, queue_bits);
    put_u64_le(header + 116, exec_bits);
    put_u32_le(header + 124, (uint32_t)body->length);

    uint32_t body_crc = wal_crc32c(0, body->data, body->length);
    put_u32_le(header + 128, body_crc);

    uint32_t header_crc = wal_crc32c(0, header, 132);
    put_u32_le(header + 132, header_crc);
}

static int snapshot_parse(const unsigned char *data,
                          size_t size,
                          const struct SnapshotMeta *expected,
                          struct SnapshotData *snapshot)
{
    if (data == NULL || snapshot == NULL ||
        size < SNAPSHOT_HEADER_SIZE)
    {
        return 0;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    if (memcmp(data, SNAPSHOT_MAGIC, 7) != 0)
    {
        log_warn("snapshot_magic_invalid",
                 "Snapshot magic does not match");
        return 0;
    }

    if (get_u16_le(data + 8) != SNAPSHOT_VERSION ||
        get_u16_le(data + 10) != SNAPSHOT_HEADER_SIZE)
    {
        log_warn("snapshot_version_invalid",
                 "Snapshot version or header size is unsupported");
        return 0;
    }

    uint32_t stored_header_crc = get_u32_le(data + 132);

    if (wal_crc32c(0, data, 132) != stored_header_crc)
    {
        log_warn("snapshot_header_crc_invalid",
                 "Snapshot header checksum does not match");
        return 0;
    }

    uint64_t lsn = get_u64_le(data + 12);
    uint64_t next_task_id = get_u64_le(data + 20);
    uint64_t created = get_u64_le(data + 28);
    uint64_t boundary_seq = get_u64_le(data + 36);
    uint64_t queued = get_u64_le(data + 44);
    uint64_t inflight = get_u64_le(data + 52);
    uint64_t history = get_u64_le(data + 60);
    uint32_t body_length = get_u32_le(data + 124);
    uint32_t body_crc = get_u32_le(data + 128);

    if (expected != NULL && expected->valid && expected->lsn != lsn)
    {
        log_warn("snapshot_lsn_mismatch",
                 "Manifest points at LSN %llu but %s contains LSN %llu",
                 (unsigned long long)expected->lsn, "snapshot",
                 (unsigned long long)lsn);
        return 0;
    }

    if ((uint64_t)body_length != (uint64_t)size - SNAPSHOT_HEADER_SIZE ||
        wal_crc32c(0, data + SNAPSHOT_HEADER_SIZE,
                   body_length) != body_crc)
    {
        log_warn("snapshot_body_crc_invalid",
                 "Snapshot body checksum does not match");
        return 0;
    }

    if (queued > SNAPSHOT_MAX_QUEUED ||
        inflight > SNAPSHOT_MAX_INFLIGHT ||
        history > SNAPSHOT_MAX_HISTORY ||
        queued > (uint64_t)body_length / 16 + 1 ||
        inflight > (uint64_t)body_length / 16 + 1 ||
        history > (uint64_t)body_length / 24 + 1)
    {
        log_warn("snapshot_count_invalid",
                 "Snapshot entry counts are not consistent with its size");
        return 0;
    }

    snapshot->lsn = lsn;
    snapshot->next_task_id = next_task_id;
    snapshot->created = created;
    snapshot->boundary_seq = boundary_seq;

    snapshot->stats.tasks_submitted =
        (int)(int64_t)get_u64_le(data + 68);
    snapshot->stats.tasks_completed =
        (int)(int64_t)get_u64_le(data + 76);
    snapshot->stats.tasks_failed =
        (int)(int64_t)get_u64_le(data + 84);
    snapshot->stats.tasks_timed_out =
        (int)(int64_t)get_u64_le(data + 92);
    snapshot->stats.tasks_unknown =
        (int)(int64_t)get_u64_le(data + 100);

    uint64_t queue_bits = get_u64_le(data + 108);
    uint64_t exec_bits = get_u64_le(data + 116);

    memcpy(&snapshot->stats.total_queue_wait_seconds,
           &queue_bits, sizeof(queue_bits));
    memcpy(&snapshot->stats.total_execution_seconds,
           &exec_bits, sizeof(exec_bits));

    if (queued > 0)
    {
        snapshot->queued = calloc((size_t)queued,
                                  sizeof(snapshot->queued[0]));
        if (snapshot->queued == NULL)
        {
            goto fail;
        }
    }

    if (inflight > 0)
    {
        snapshot->inflight = calloc((size_t)inflight,
                                    sizeof(snapshot->inflight[0]));
        if (snapshot->inflight == NULL)
        {
            goto fail;
        }
    }

    if (history > 0)
    {
        snapshot->history = calloc((size_t)history,
                                   sizeof(snapshot->history[0]));
        if (snapshot->history == NULL)
        {
            goto fail;
        }
    }

    struct Reader reader = {
        .data = data + SNAPSHOT_HEADER_SIZE,
        .length = body_length,
        .offset = 0,
        .failed = 0
    };

    for (uint64_t i = 0; i < queued; i++)
    {
        if (!snapshot_read_task(&reader, &snapshot->queued[i]))
        {
            goto fail;
        }
    }

    for (uint64_t i = 0; i < inflight; i++)
    {
        if (!snapshot_read_task(&reader, &snapshot->inflight[i].task))
        {
            goto fail;
        }

        snapshot->inflight[i].worker_id =
            (int)reader_u32(&reader);
        snapshot->inflight[i].dispatched_at =
            (time_t)reader_i64(&reader);
        snapshot->inflight[i].accepted = reader_u8(&reader) != 0;

        if (reader.failed)
        {
            goto fail;
        }
    }

    for (uint64_t i = 0; i < history; i++)
    {
        snapshot->history[i].task_id =
            (int32_t)reader_u32(&reader);
        snapshot->history[i].worker_id =
            (int32_t)reader_u32(&reader);
        snapshot->history[i].exit_status =
            (int32_t)reader_u32(&reader);
        snapshot->history[i].state =
            (int32_t)reader_u32(&reader);
        snapshot->history[i].completed_at =
            (time_t)reader_i64(&reader);
    }

    if (reader.failed || reader.offset != reader.length)
    {
        goto fail;
    }

    snapshot->queued_count = (size_t)queued;
    snapshot->inflight_count = (size_t)inflight;
    snapshot->history_count = (size_t)history;
    snapshot->valid = 1;

    return 1;

fail:
    log_warn("snapshot_parse_failed",
             "Snapshot body is truncated or malformed");
    snapshot_data_free(snapshot);
    return 0;
}

static int load_snapshot(const char *base_path,
                         const struct SnapshotMeta *expected,
                         struct SnapshotData *snapshot)
{
    uint64_t lsn = expected != NULL && expected->valid
                       ? expected->lsn
                       : 0;

    if (expected == NULL || !expected->valid)
    {
        return 0;
    }

    char path[WAL_PATH_MAX];

    if (!wal_snapshot_path(base_path, lsn, path, sizeof(path)))
    {
        return 0;
    }

    unsigned char *data = NULL;
    size_t size = 0;

    if (!read_entire_file(path, &data, &size))
    {
        log_warn("snapshot_read_failed",
                 "Could not read snapshot %s", path);
        return 0;
    }

    int ok = snapshot_parse(data, size, expected, snapshot);

    free(data);

    return ok;
}

static int read_manifest(const char *base_path,
                         struct Manifest *manifest)
{
    memset(manifest, 0, sizeof(*manifest));

    char path[WAL_PATH_MAX];

    if (!wal_manifest_path(base_path, path, sizeof(path)))
    {
        return 0;
    }

    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        return 1;
    }

    char line[512];

    if (fgets(line, sizeof(line), file) == NULL ||
        strncmp(line, "IDRMAN2", 7) != 0)
    {
        fclose(file);
        log_warn("manifest_header_invalid",
                 "Manifest at %s is invalid; ignoring it", path);
        return 1;
    }

    while (fgets(line, sizeof(line), file) != NULL)
    {
        unsigned long long lsn = 0;
        unsigned long long boundary = 0;
        unsigned long long created = 0;

        if (sscanf(line, "current %llu %llu %llu",
                   &lsn, &boundary, &created) == 3)
        {
            manifest->current.lsn = (uint64_t)lsn;
            manifest->current.boundary_seq = (uint64_t)boundary;
            manifest->current.created = (uint64_t)created;
            manifest->current.valid = 1;
        }
        else if (sscanf(line, "previous %llu %llu %llu",
                        &lsn, &boundary, &created) == 3)
        {
            manifest->previous.lsn = (uint64_t)lsn;
            manifest->previous.boundary_seq = (uint64_t)boundary;
            manifest->previous.created = (uint64_t)created;
            manifest->previous.valid = 1;
        }
    }

    fclose(file);

    return 1;
}

static int write_manifest(const char *base_path,
                          const struct Manifest *manifest)
{
    char path[WAL_PATH_MAX];
    char tmp_path[WAL_PATH_MAX + 8];

    if (!wal_manifest_path(base_path, path, sizeof(path)))
    {
        return 0;
    }

    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    if (n <= 0 || (size_t)n >= sizeof(tmp_path))
    {
        return 0;
    }

    char text[512];
    int length = 0;

    length += snprintf(text + length, sizeof(text) - (size_t)length,
                       "IDRMAN2\n");

    if (manifest->current.valid)
    {
        length += snprintf(text + length, sizeof(text) - (size_t)length,
                           "current %llu %llu %llu\n",
                           (unsigned long long)manifest->current.lsn,
                           (unsigned long long)manifest->current.boundary_seq,
                           (unsigned long long)manifest->current.created);
    }
    else
    {
        length += snprintf(text + length, sizeof(text) - (size_t)length,
                           "current none\n");
    }

    if (manifest->previous.valid)
    {
        length += snprintf(text + length, sizeof(text) - (size_t)length,
                           "previous %llu %llu %llu\n",
                           (unsigned long long)manifest->previous.lsn,
                           (unsigned long long)manifest->previous.boundary_seq,
                           (unsigned long long)manifest->previous.created);
    }
    else
    {
        length += snprintf(text + length, sizeof(text) - (size_t)length,
                           "previous none\n");
    }

    if (length <= 0 || (size_t)length >= sizeof(text))
    {
        return 0;
    }

    if (!write_file_atomic(path, tmp_path, text, (size_t)length))
    {
        return 0;
    }

    idr_test_crash_point("manifest_after_rename");

    return 1;
}

static void cleanup_snapshots(const char *base_path,
                              uint64_t keep_lsn_a,
                              uint64_t keep_lsn_b,
                              int keep_second)
{
    char pattern[WAL_PATH_MAX];

    int n = snprintf(pattern, sizeof(pattern),
                     "%s.snap.*", base_path);

    if (n <= 0 || (size_t)n >= sizeof(pattern))
    {
        return;
    }

    glob_t matches;

    memset(&matches, 0, sizeof(matches));

    int rc = glob(pattern, 0, NULL, &matches);

    if (rc != 0)
    {
        globfree(&matches);
        return;
    }

    for (size_t i = 0; i < matches.gl_pathc; i++)
    {
        const char *path = matches.gl_pathv[i];
        size_t length = strlen(path);

        if (length >= 4 &&
            strcmp(path + length - 4, ".tmp") == 0)
        {
            unlink(path);
            continue;
        }

        uint64_t lsn = 0;

        if (!parse_snapshot_lsn(path, &lsn))
        {
            continue;
        }

        if (lsn == keep_lsn_a ||
            (keep_second && lsn == keep_lsn_b))
        {
            continue;
        }

        unlink(path);
    }

    globfree(&matches);
}

int recovery_take_snapshot(struct Runtime *runtime,
                           struct InFlightTable *table,
                           struct Wal *wal)
{
    if (runtime == NULL || runtime->task_manager == NULL ||
        table == NULL || wal == NULL || wal->fd < 0)
    {
        return 0;
    }

    uint64_t boundary_seq = wal->active_seq;

    if (!wal_rotate(wal))
    {
        log_error("snapshot_rotate_failed",
                  "Could not rotate the WAL before snapshot");
        return 0;
    }

    struct SnapshotData snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    snapshot.lsn = wal->next_lsn == 0 ? 0 : wal->next_lsn - 1;
    snapshot.next_task_id = (uint64_t)table->next_task_id;
    snapshot.created = (uint64_t)time(NULL);
    snapshot.boundary_seq = boundary_seq;
    snapshot.stats = runtime->stats;
    snapshot.queued_count =
        (size_t)runtime->task_manager->queue.count;
    snapshot.history_count =
        coordinator_history_count(&table->history);

    for (size_t i = 0; i < table->capacity; i++)
    {
        if (table->entries[i].in_use)
        {
            snapshot.inflight_count++;
        }
    }

    struct Buffer body;
    memset(&body, 0, sizeof(body));

    for (size_t i = 0; i < snapshot.queued_count; i++)
    {
        struct Task task;

        if (!task_queue_peek(&runtime->task_manager->queue,
                             (int)i, &task))
        {
            buffer_free(&body);
            log_error("snapshot_queue_peek_failed",
                      "Could not read queued task %zu", i);
            return 0;
        }

        snapshot_write_task(&body, &task);
    }

    size_t written_inflight = 0;

    for (size_t i = 0; i < table->capacity; i++)
    {
        if (!table->entries[i].in_use)
        {
            continue;
        }

        snapshot_write_task(&body, &table->entries[i].task);
        buffer_u32(&body, (uint32_t)table->entries[i].worker_id);
        buffer_i64(&body, (int64_t)table->entries[i].dispatched_at);
        buffer_u8(&body,
                  table->entries[i].accepted ? 1 : 0);
        written_inflight++;
    }

    for (size_t i = 0; i < snapshot.history_count; i++)
    {
        struct HistoryRecord record;

        if (!coordinator_history_get_by_index(&table->history, i, &record))
        {
            buffer_free(&body);
            log_error("snapshot_history_get_failed",
                      "Could not read retained result %zu", i);
            return 0;
        }

        buffer_u32(&body, (uint32_t)record.task_id);
        buffer_u32(&body, (uint32_t)record.worker_id);
        buffer_u32(&body, (uint32_t)record.exit_status);
        buffer_u32(&body, (uint32_t)record.state);
        buffer_i64(&body, (int64_t)record.completed_at);
    }

    if (body.failed || written_inflight != snapshot.inflight_count)
    {
        buffer_free(&body);
        return 0;
    }

    unsigned char header[SNAPSHOT_HEADER_SIZE];
    snapshot_build_header(header, &snapshot, &body);

    char path[WAL_PATH_MAX];
    char tmp_path[WAL_PATH_MAX + 8];

    if (!wal_snapshot_path(wal->base_path, snapshot.lsn,
                           path, sizeof(path)))
    {
        buffer_free(&body);
        return 0;
    }

    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    if (n <= 0 || (size_t)n >= sizeof(tmp_path))
    {
        buffer_free(&body);
        return 0;
    }

    struct Buffer file;
    memset(&file, 0, sizeof(file));

    buffer_put(&file, header, sizeof(header));
    buffer_put(&file, body.data, body.length);
    buffer_free(&body);

    if (file.failed)
    {
        buffer_free(&file);
        return 0;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
    {
        log_error("snapshot_open_failed",
                  "Could not open %s: %s", tmp_path, strerror(errno));
        buffer_free(&file);
        return 0;
    }

    int write_ok = write_all(fd, file.data, file.length) &&
                   fsync(fd) == 0;

    buffer_free(&file);

    if (close(fd) != 0)
    {
        write_ok = 0;
    }

    if (!write_ok)
    {
        log_error("snapshot_write_failed",
                  "Could not write snapshot %s: %s",
                  tmp_path, strerror(errno));
        unlink(tmp_path);
        return 0;
    }

    idr_test_crash_point("snapshot_after_tmp_write");

    if (rename(tmp_path, path) != 0)
    {
        log_error("snapshot_rename_failed",
                  "Could not rename %s to %s: %s",
                  tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return 0;
    }

    if (!wal_fsync_directory(path))
    {
        log_warn("snapshot_dir_fsync_failed",
                 "Could not fsync directory for %s", path);
    }

    idr_test_crash_point("snapshot_after_rename");

    struct Manifest old_manifest;
    read_manifest(wal->base_path, &old_manifest);

    struct Manifest next_manifest;
    memset(&next_manifest, 0, sizeof(next_manifest));

    next_manifest.current.lsn = snapshot.lsn;
    next_manifest.current.boundary_seq = snapshot.boundary_seq;
    next_manifest.current.created = snapshot.created;
    next_manifest.current.valid = 1;

    if (old_manifest.current.valid)
    {
        next_manifest.previous = old_manifest.current;
    }
    else if (old_manifest.previous.valid)
    {
        next_manifest.previous = old_manifest.previous;
    }

    if (!write_manifest(wal->base_path, &next_manifest))
    {
        log_error("manifest_write_failed",
                  "Snapshot %s was written but the manifest was not "
                  "updated; the previous manifest remains valid",
                  path);
        return 0;
    }

    cleanup_snapshots(wal->base_path,
                      next_manifest.current.lsn,
                      next_manifest.previous.lsn,
                      next_manifest.previous.valid);

    uint64_t keep_from = next_manifest.previous.valid
                             ? next_manifest.previous.boundary_seq
                             : wal->active_seq;

    if (wal_remove_segments_before(wal->base_path, keep_from) < 0)
    {
        log_warn("snapshot_segment_truncate_failed",
                 "Could not truncate old WAL segments");
    }

    wal->records_since_snapshot = 0;
    wal->last_snapshot_time = time(NULL);

    log_info("snapshot_written",
             "Snapshot written: lsn=%llu boundary_seq=%llu "
             "queued=%zu inflight=%zu retained=%zu",
             (unsigned long long)snapshot.lsn,
             (unsigned long long)snapshot.boundary_seq,
             snapshot.queued_count,
             snapshot.inflight_count,
             snapshot.history_count);

    return 1;
}

struct RecoveryEntry
{
    int task_id;
    struct Task task;
    int has_submit;
    int dispatched;
    int accepted;
    int last_worker;
    time_t dispatched_at;
};

struct RecoveryMap
{
    struct RecoveryEntry *entries;
    size_t capacity;
    size_t count;
    size_t tombstones;
};

struct ReplayResult
{
    int task_id;
    int worker_id;
    int exit_status;
    int state;
};

struct RecoveryState
{
    struct RecoveryMap map;

    struct ReplayResult *results;
    size_t result_count;
    size_t result_capacity;

    uint64_t max_lsn;
    int max_task_id;
    int submit_count;
    int overflow;
};

static size_t recovery_hash(int task_id, size_t mask)
{
    uint32_t x = (uint32_t)task_id;

    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;

    return (size_t)x & mask;
}

static void recovery_map_free(struct RecoveryMap *map)
{
    free(map->entries);
    memset(map, 0, sizeof(*map));
}

static int recovery_map_rehash(struct RecoveryMap *map,
                               size_t capacity)
{
    struct RecoveryEntry *entries =
        calloc(capacity, sizeof(entries[0]));

    if (entries == NULL)
    {
        return 0;
    }

    struct RecoveryEntry *old = map->entries;
    size_t old_capacity = map->capacity;

    map->entries = entries;
    map->capacity = capacity;
    map->count = 0;
    map->tombstones = 0;

    for (size_t i = 0; i < old_capacity; i++)
    {
        if (old[i].task_id <= 0)
        {
            continue;
        }

        size_t mask = capacity - 1;
        size_t index = recovery_hash(old[i].task_id, mask);

        while (entries[index].task_id != 0)
        {
            index = (index + 1) & mask;
        }

        entries[index] = old[i];
        map->count++;
    }

    free(old);

    return 1;
}

static int recovery_map_reserve(struct RecoveryMap *map)
{
    if (map->capacity == 0)
    {
        return recovery_map_rehash(map, 256);
    }

    if ((map->count + map->tombstones + 1) * 10 <
        map->capacity * 7)
    {
        return 1;
    }

    if (map->capacity > SIZE_MAX / 2)
    {
        return 0;
    }

    return recovery_map_rehash(map, map->capacity * 2);
}

static struct RecoveryEntry *recovery_map_find(
    struct RecoveryMap *map,
    int task_id)
{
    if (map->capacity == 0 || map->entries == NULL)
    {
        return NULL;
    }

    size_t mask = map->capacity - 1;
    size_t index = recovery_hash(task_id, mask);

    for (size_t probe = 0; probe <= mask; probe++)
    {
        struct RecoveryEntry *entry = &map->entries[index];

        if (entry->task_id == 0)
        {
            return NULL;
        }

        if (entry->task_id == task_id)
        {
            return entry;
        }

        index = (index + 1) & mask;
    }

    return NULL;
}

static struct RecoveryEntry *recovery_map_insert(
    struct RecoveryMap *map,
    int task_id)
{
    struct RecoveryEntry *existing = recovery_map_find(map, task_id);

    if (existing != NULL)
    {
        return existing;
    }

    if (!recovery_map_reserve(map))
    {
        return NULL;
    }

    size_t mask = map->capacity - 1;
    size_t index = recovery_hash(task_id, mask);
    size_t first_tombstone = SIZE_MAX;

    for (size_t probe = 0; probe <= mask; probe++)
    {
        struct RecoveryEntry *entry = &map->entries[index];

        if (entry->task_id == 0)
        {
            size_t target = first_tombstone != SIZE_MAX
                                ? first_tombstone
                                : index;

            entry = &map->entries[target];
            memset(entry, 0, sizeof(*entry));
            entry->task_id = task_id;
            map->count++;

            if (target != index)
            {
                map->tombstones--;
            }

            return entry;
        }

        if (entry->task_id == -1 && first_tombstone == SIZE_MAX)
        {
            first_tombstone = index;
        }

        index = (index + 1) & mask;
    }

    return NULL;
}

static void recovery_map_remove(struct RecoveryMap *map,
                                int task_id)
{
    struct RecoveryEntry *entry = recovery_map_find(map, task_id);

    if (entry == NULL)
    {
        return;
    }

    entry->task_id = -1;
    map->count--;
    map->tombstones++;
}

static int recovery_results_push(struct RecoveryState *state,
                                 int task_id,
                                 int worker_id,
                                 int exit_status,
                                 int state_value)
{
    if (state->result_count == state->result_capacity)
    {
        size_t capacity = state->result_capacity == 0
                              ? 256
                              : state->result_capacity * 2;

        if (capacity > 10000000u)
        {
            state->overflow = 1;
            return 0;
        }

        struct ReplayResult *results =
            realloc(state->results,
                    capacity * sizeof(results[0]));

        if (results == NULL)
        {
            state->overflow = 1;
            return 0;
        }

        state->results = results;
        state->result_capacity = capacity;
    }

    state->results[state->result_count].task_id = task_id;
    state->results[state->result_count].worker_id = worker_id;
    state->results[state->result_count].exit_status = exit_status;
    state->results[state->result_count].state = state_value;
    state->result_count++;

    return 1;
}

static void recovery_state_free(struct RecoveryState *state)
{
    recovery_map_free(&state->map);
    free(state->results);
    memset(state, 0, sizeof(*state));
}

static int parse_submit_record(const char *line,
                               int *task_id_out,
                               struct Task *task_out)
{
    const char *prefix = "SUBMIT|";
    size_t prefix_length = strlen(prefix);

    if (strncmp(line, prefix, prefix_length) != 0)
    {
        return 0;
    }

    const char *cursor = line + prefix_length;

    long task_id = 0;

    if (!wire_read_int(&cursor, &task_id) || task_id <= 0 ||
        task_id > INT_MAX)
    {
        return 0;
    }

    if (!wire_read_delimiter(&cursor))
    {
        return 0;
    }

    struct Task task;
    memset(&task, 0, sizeof(task));
    task.id = (int)task_id;
    task.assigned_worker_id = -1;
    task.state = TASK_CREATED;

    if (!wire_read_field(&cursor,
                         task.command,
                         sizeof(task.command)))
    {
        return 0;
    }

    if (!wire_read_delimiter(&cursor))
    {
        return 0;
    }

    long argc = 0;

    if (!wire_read_int(&cursor, &argc))
    {
        return 0;
    }

    if (argc < 0 || argc > TASK_ARGUMENT_COUNT)
    {
        return 0;
    }

    for (int i = 0; i < (int)argc; i++)
    {
        if (!wire_read_delimiter(&cursor))
        {
            return 0;
        }

        if (!wire_read_field(&cursor,
                             task.arguments[i],
                             sizeof(task.arguments[i])))
        {
            return 0;
        }
    }

    task.argument_count = (int)argc;

    if (*cursor == '|')
    {
        cursor++;

        long idem = 0;

        if (wire_read_int(&cursor, &idem))
        {
            task.idempotent = idem ? 1 : 0;
        }
    }

    if (*cursor == '|')
    {
        cursor++;

        long timeout = 0;

        if (!wire_read_int(&cursor, &timeout))
        {
            log_warn("wal_submit_bad_timeout",
                     "Malformed timeout field in SUBMIT record; "
                     "treating as no timeout");
        }
        else if (timeout < 0 || timeout > INT_MAX)
        {
            log_warn("wal_submit_timeout_out_of_range",
                     "Timeout %ld out of range in SUBMIT record; "
                     "treating as no timeout",
                     timeout);
        }
        else
        {
            task.timeout_seconds = (int)timeout;
        }
    }

    *task_id_out = (int)task_id;
    *task_out = task;

    return 1;
}

static int recovery_record_cb(enum WalRecordType type,
                              uint64_t lsn,
                              const char *payload,
                              void *user)
{
    struct RecoveryState *state = user;

    if (lsn > state->max_lsn)
    {
        state->max_lsn = lsn;
    }

    if (type == WAL_RECORD_SUBMIT)
    {
        int task_id = 0;
        struct Task task;

        if (!parse_submit_record(payload, &task_id, &task))
        {
            log_warn("wal_submit_invalid",
                     "Skipping malformed SUBMIT record: %s", payload);
            return 1;
        }

        struct RecoveryEntry *entry =
            recovery_map_insert(&state->map, task_id);

        if (entry == NULL)
        {
            state->overflow = 1;
            return 0;
        }

        int previous_attempt = entry->task.attempt;

        entry->task = task;
        entry->task.attempt = previous_attempt;
        entry->has_submit = 1;

        if (task_id > state->max_task_id)
        {
            state->max_task_id = task_id;
        }

        state->submit_count++;

        return 1;
    }

    if (type == WAL_RECORD_DISPATCH || type == WAL_RECORD_ACCEPT)
    {
        int task_id = 0;
        int worker_id = 0;
        int epoch = 0;
        int attempt = 0;
        long long timestamp = 0;

        const char *format = type == WAL_RECORD_DISPATCH
                                 ? "DISPATCH|%d|%d|%d|%d|%lld"
                                 : "ACCEPT|%d|%d|%d|%d|%lld";

        if (sscanf(payload, format,
                   &task_id, &worker_id, &epoch,
                   &attempt, &timestamp) != 5 ||
            task_id <= 0)
        {
            log_warn("wal_dispatch_invalid",
                     "Skipping malformed %s record: %s",
                     type == WAL_RECORD_DISPATCH
                         ? "DISPATCH" : "ACCEPT",
                     payload);
            return 1;
        }

        (void)epoch;

        struct RecoveryEntry *entry =
            recovery_map_insert(&state->map, task_id);

        if (entry == NULL)
        {
            state->overflow = 1;
            return 0;
        }

        entry->task.id = task_id;
        entry->task.assigned_worker_id = worker_id;
        entry->task.attempt = attempt > 0 ? attempt : 1;
        entry->dispatched = 1;
        entry->last_worker = worker_id;
        entry->dispatched_at = timestamp > 0
                                   ? (time_t)timestamp
                                   : time(NULL);

        if (type == WAL_RECORD_ACCEPT)
        {
            entry->accepted = 1;
        }

        if (task_id > state->max_task_id)
        {
            state->max_task_id = task_id;
        }

        return 1;
    }

    if (type == WAL_RECORD_RESULT)
    {
        int task_id = 0;
        int worker_id = 0;
        int exit_status = 0;
        int state_value = 0;

        if (sscanf(payload, "RESULT|%d|%d|%d|%d",
                   &task_id, &worker_id,
                   &exit_status, &state_value) != 4 ||
            task_id <= 0)
        {
            log_warn("wal_result_invalid",
                     "Skipping malformed RESULT record: %s", payload);
            return 1;
        }

        if (state_value < TASK_COMPLETED ||
            state_value > TASK_UNKNOWN)
        {
            log_warn("wal_result_state_invalid",
                     "RESULT for task %d has invalid state %d; "
                     "recording it as TASK_FAILED",
                     task_id, state_value);
            state_value = TASK_FAILED;
        }

        if (!recovery_results_push(state,
                                   task_id,
                                   worker_id,
                                   exit_status,
                                   state_value))
        {
            return 0;
        }

        recovery_map_remove(&state->map, task_id);

        if (task_id > state->max_task_id)
        {
            state->max_task_id = task_id;
        }

        return 1;
    }

    log_warn("wal_record_unknown",
             "Skipping unknown WAL record type %d", (int)type);

    return 1;
}

static int legacy_line_cb(const char *line, void *user)
{
    struct RecoveryState *state = user;

    if (strncmp(line, "SUBMIT|", 7) == 0)
    {
        return recovery_record_cb(WAL_RECORD_SUBMIT, 0, line, state);
    }

    if (strncmp(line, "RESULT|", 7) == 0)
    {
        return recovery_record_cb(WAL_RECORD_RESULT, 0, line, state);
    }

    if (strncmp(line, "DISPATCH|", 9) == 0)
    {
        return recovery_record_cb(WAL_RECORD_DISPATCH, 0, line, state);
    }

    if (strncmp(line, "ACCEPT|", 7) == 0)
    {
        return recovery_record_cb(WAL_RECORD_ACCEPT, 0, line, state);
    }

    log_warn("wal_unknown_record",
             "Skipping unknown legacy WAL record: %s", line);

    return 1;
}

static void apply_snapshot(struct SnapshotData *snapshot,
                           struct RecoveryState *state)
{
    if (snapshot == NULL || !snapshot->valid)
    {
        return;
    }

    for (size_t i = 0; i < snapshot->queued_count; i++)
    {
        struct RecoveryEntry *entry =
            recovery_map_insert(&state->map,
                                snapshot->queued[i].id);

        if (entry == NULL)
        {
            state->overflow = 1;
            return;
        }

        entry->task = snapshot->queued[i];
        entry->has_submit = 1;
    }

    for (size_t i = 0; i < snapshot->inflight_count; i++)
    {
        struct RecoveryEntry *entry =
            recovery_map_insert(&state->map,
                                snapshot->inflight[i].task.id);

        if (entry == NULL)
        {
            state->overflow = 1;
            return;
        }

        entry->task = snapshot->inflight[i].task;
        entry->task.assigned_worker_id =
            snapshot->inflight[i].worker_id;
        entry->has_submit = 1;
        entry->dispatched = 1;
        entry->accepted = snapshot->inflight[i].accepted;
        entry->last_worker = snapshot->inflight[i].worker_id;
        entry->dispatched_at = snapshot->inflight[i].dispatched_at;
    }

    if (snapshot->next_task_id > 0 &&
        (int)snapshot->next_task_id - 1 > state->max_task_id)
    {
        state->max_task_id = (int)snapshot->next_task_id - 1;
    }
}

static void record_result_stats(struct Runtime *runtime, int state_value)
{
    if (state_value == TASK_COMPLETED)
    {
        record_task_completed(&runtime->stats, 0.0, 0.0);
    }
    else if (state_value == TASK_TIMED_OUT)
    {
        record_task_timed_out(&runtime->stats);
    }
    else if (state_value == TASK_UNKNOWN)
    {
        record_task_unknown(&runtime->stats);
    }
    else
    {
        record_task_failed(&runtime->stats);
    }
}

static int enqueue_recovered_task(struct Runtime *runtime,
                                  const struct Task *source)
{
    struct Task task = *source;

    task.assigned_worker_id = -1;

    if (task.state == TASK_RUNNING)
    {
        task.state = TASK_CREATED;
    }

    initialize_task_metrics(&task.metrics);
    record_task_submission(&task.metrics);

    return submit_task(runtime->task_manager, &task);
}

static int materialize_recovery(struct Runtime *runtime,
                                struct InFlightTable *table,
                                struct RecoveryState *state)
{
    int requeued = 0;
    int failed = 0;
    int unknown = 0;

    for (size_t i = 0; i < state->map.capacity; i++)
    {
        struct RecoveryEntry *entry = &state->map.entries[i];

        if (entry->task_id <= 0)
        {
            continue;
        }

        /*
         * A retained RESULT means the task is already terminal.
         * This happens when a SUBMIT record is replayed after an
         * earlier attempt's RESULT (for example, a duplicate
         * submission that was accepted before a crash).
         */
        if (coordinator_lookup_completed(table,
                                           entry->task_id,
                                           NULL, NULL, NULL))
        {
            continue;
        }

        if (!entry->dispatched)
        {
            if (!enqueue_recovered_task(runtime, &entry->task))
            {
                log_error("recovery_queue_overflow",
                          "Could not queue recovered task %d; the "
                          "task queue is full",
                          entry->task_id);
                return 0;
            }

            requeued++;
            continue;
        }

        int terminal_state = -1;

        if (entry->task.idempotent)
        {
            if (entry->task.attempt >= COORDINATOR_MAX_ATTEMPTS)
            {
                terminal_state = TASK_FAILED;
            }
            else
            {
                /*
                 * Keep the recorded attempt count. The next
                 * dispatch increments it; incrementing here
                 * would count one attempt twice and exhaust
                 * the retry budget early.
                 */
                struct Task task = entry->task;
                task.state = TASK_CREATED;
                task.assigned_worker_id = -1;

                if (!enqueue_recovered_task(runtime, &task))
                {
                    log_error("recovery_queue_overflow",
                              "Could not requeue idempotent task %d",
                              entry->task_id);
                    return 0;
                }

                requeued++;
                continue;
            }
        }
        else
        {
            terminal_state = TASK_UNKNOWN;
        }

        const char *reason = terminal_state == TASK_UNKNOWN
                                 ? "unknown_after_recovery"
                                 : "retry_exhausted";

        char line[128];
        int n = snprintf(line, sizeof(line),
                         "RESULT|%d|%d|-1|%d",
                         entry->task_id,
                         entry->last_worker > 0
                             ? entry->last_worker
                             : entry->task.assigned_worker_id,
                         terminal_state);

        if (n <= 0 || (size_t)n >= sizeof(line) ||
            !wal_append(&runtime->wal, line))
        {
            log_error("recovery_result_append_failed",
                      "Could not persist the %s result for task %d",
                      reason, entry->task_id);
            return 0;
        }

        coordinator_record_completed(table,
                                     entry->task_id,
                                     entry->last_worker,
                                     -1,
                                     terminal_state);

        record_result_stats(runtime, terminal_state);

        if (terminal_state == TASK_UNKNOWN)
        {
            unknown++;
            log_warn("task_unknown_recovered",
                     "Task %d is non-idempotent and was dispatched but "
                     "has no result; recording TASK_UNKNOWN",
                     entry->task_id);
        }
        else
        {
            failed++;
            log_warn("task_retry_exhausted",
                     "Task %d exhausted %d attempts; recording "
                     "TASK_FAILED",
                     entry->task_id,
                     COORDINATOR_MAX_ATTEMPTS);
        }
    }

    log_info("recovery_tasks_materialized",
             "Requeued %d, failed %d, unknown %d",
             requeued, failed, unknown);

    return 1;
}

static int import_legacy_wal(const char *base_path,
                             struct RecoveryState *state)
{
    if (access(base_path, F_OK) != 0)
    {
        return 1;
    }

    int lines = wal_foreach_line(base_path, legacy_line_cb, state);

    if (lines < 0 || state->overflow)
    {
        log_error("wal_legacy_import_failed",
                  "Could not import legacy WAL at %s", base_path);
        return 0;
    }

    char legacy_path[WAL_PATH_MAX + 64];
    int n = snprintf(legacy_path, sizeof(legacy_path),
                     "%s.legacy", base_path);

    if (n <= 0 || (size_t)n >= sizeof(legacy_path))
    {
        return 0;
    }

    if (access(legacy_path, F_OK) == 0)
    {
        n = snprintf(legacy_path, sizeof(legacy_path),
                     "%s.legacy.%ld", base_path, (long)time(NULL));

        if (n <= 0 || (size_t)n >= sizeof(legacy_path))
        {
            return 0;
        }
    }

    if (rename(base_path, legacy_path) != 0)
    {
        log_error("wal_legacy_rename_failed",
                  "Imported %d legacy record(s) but could not rename "
                  "%s to %s: %s. Refusing to start to avoid a double "
                  "import.",
                  lines, base_path, legacy_path, strerror(errno));
        return 0;
    }

    log_info("wal_legacy_imported",
             "Imported %d record(s) from legacy WAL; moved it to %s",
             lines, legacy_path);

    return 1;
}

int recovery_snapshot_due(const struct Wal *wal)
{
    return wal_should_snapshot(wal);
}

int recovery_start(struct Runtime *runtime,
                   struct InFlightTable *table,
                   const char *wal_base_path)
{
    if (runtime == NULL || table == NULL ||
        runtime->task_manager == NULL ||
        wal_base_path == NULL)
    {
        return 0;
    }

    uint64_t segment_sequence = 0;
    int have_segment =
        wal_find_active_segment(wal_base_path, &segment_sequence);

    if (have_segment < 0)
    {
        log_error("wal_segment_list_failed",
                  "Could not list WAL segments for %s",
                  wal_base_path);
        return 0;
    }

    char manifest_path[WAL_PATH_MAX];

    int have_manifest =
        wal_manifest_path(wal_base_path,
                          manifest_path,
                          sizeof(manifest_path)) &&
        access(manifest_path, F_OK) == 0;

    uint64_t latest_snapshot_lsn = 0;

    int have_snapshot =
        find_latest_snapshot(wal_base_path, &latest_snapshot_lsn);

    int v2_exists = have_segment || have_manifest || have_snapshot;

    struct Manifest manifest;
    read_manifest(wal_base_path, &manifest);

    struct SnapshotData snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    int snapshot_loaded = 0;

    if (manifest.current.valid)
    {
        snapshot_loaded = load_snapshot(wal_base_path,
                                        &manifest.current,
                                        &snapshot);

        if (!snapshot_loaded && manifest.previous.valid)
        {
            log_warn("snapshot_fallback",
                     "Latest snapshot is unreadable; trying the "
                     "previous snapshot at LSN %llu",
                     (unsigned long long)manifest.previous.lsn);

            snapshot_loaded = load_snapshot(wal_base_path,
                                            &manifest.previous,
                                            &snapshot);
        }

        if (!snapshot_loaded)
        {
            log_error("snapshot_unreadable",
                      "Both the latest and previous snapshots are "
                      "unreadable; refusing to start");
            return 0;
        }
    }
    else if (manifest.previous.valid)
    {
        snapshot_loaded = load_snapshot(wal_base_path,
                                        &manifest.previous,
                                        &snapshot);

        if (!snapshot_loaded)
        {
            log_error("snapshot_unreadable",
                      "Manifest previous snapshot is unreadable; "
                      "refusing to start");
            return 0;
        }
    }
    else if (have_snapshot)
    {
        struct SnapshotMeta meta = {
            .lsn = latest_snapshot_lsn,
            .valid = 1
        };

        snapshot_loaded = load_snapshot(wal_base_path, &meta, &snapshot);

        if (snapshot_loaded)
        {
            log_info("snapshot_orphan_loaded",
                     "Loaded snapshot at LSN %llu without a manifest",
                     (unsigned long long)snapshot.lsn);
        }
    }

    /*
     * A snapshot is only meaningful if every segment boundary it
     * depends on still exists. Without this check, a deleted segment
     * would silently drop acknowledged tasks and recovery would
     * report success with incomplete state.
     */
    if (snapshot_loaded &&
        !wal_validate_segment_chain(wal_base_path, snapshot.boundary_seq))
    {
        log_error("wal_segment_chain_invalid",
                  "WAL segments required by snapshot LSN %llu are "
                  "missing or non-contiguous; refusing to start",
                  (unsigned long long)snapshot.lsn);
        snapshot_data_free(&snapshot);
        return 0;
    }

    struct RecoveryState state;
    memset(&state, 0, sizeof(state));

    if (snapshot_loaded)
    {
        apply_snapshot(&snapshot, &state);

        if (state.overflow)
        {
            log_error("recovery_map_overflow",
                      "Could not build the recovery map from snapshot");
            snapshot_data_free(&snapshot);
            recovery_state_free(&state);
            return 0;
        }
    }

    uint64_t replay_after = snapshot_loaded ? snapshot.lsn : 0;

    int records = wal_foreach_record(wal_base_path,
                                     replay_after,
                                     recovery_record_cb,
                                     &state);

    if (records < 0 || state.overflow)
    {
        log_error("wal_replay_failed",
                  "Could not replay WAL at %s; refusing to start",
                  wal_base_path);
        snapshot_data_free(&snapshot);
        recovery_state_free(&state);
        return 0;
    }

    if (!v2_exists)
    {
        if (!import_legacy_wal(wal_base_path, &state))
        {
            snapshot_data_free(&snapshot);
            recovery_state_free(&state);
            return 0;
        }
    }
    else if (access(wal_base_path, F_OK) == 0)
    {
        log_warn("wal_legacy_ignored",
                 "A legacy WAL exists at %s but v2 WAL data is also "
                 "present; the legacy file is left untouched",
                 wal_base_path);
    }

    if (!wal_open_for_append(&runtime->wal, wal_base_path))
    {
        log_error("wal_open_failed",
                  "Could not open WAL at %s", wal_base_path);
        snapshot_data_free(&snapshot);
        recovery_state_free(&state);
        return 0;
    }

    if (state.max_lsn + 1 > runtime->wal.next_lsn)
    {
        runtime->wal.next_lsn = state.max_lsn + 1;
    }

    if (snapshot_loaded && snapshot.lsn + 1 > runtime->wal.next_lsn)
    {
        runtime->wal.next_lsn = snapshot.lsn + 1;
    }

    if (snapshot_loaded)
    {
        runtime->stats = snapshot.stats;
    }

    if (state.submit_count > 0)
    {
        runtime->stats.tasks_submitted += state.submit_count;
    }

    if (snapshot_loaded)
    {
        for (size_t i = 0; i < snapshot.history_count; i++)
        {
            struct HistoryRecord *record = &snapshot.history[i];

            coordinator_record_completed_at(table,
                                            record->task_id,
                                            record->worker_id,
                                            record->exit_status,
                                            record->state,
                                            record->completed_at);
        }
    }

    for (size_t i = 0; i < state.result_count; i++)
    {
        struct ReplayResult *result = &state.results[i];

        if (coordinator_lookup_completed(table,
                                           result->task_id,
                                           NULL, NULL, NULL))
        {
            continue;
        }

        coordinator_record_completed_at(table,
                                        result->task_id,
                                        result->worker_id,
                                        result->exit_status,
                                        result->state,
                                        time(NULL));

        record_result_stats(runtime, result->state);
    }

    if (!materialize_recovery(runtime, table, &state))
    {
        wal_close(&runtime->wal);
        snapshot_data_free(&snapshot);
        recovery_state_free(&state);
        return 0;
    }

    if (state.max_task_id >= INT_MAX)
    {
        log_error("task_id_exhausted",
                  "Recovered task ID space is exhausted");
        wal_close(&runtime->wal);
        snapshot_data_free(&snapshot);
        recovery_state_free(&state);
        return 0;
    }

    int next_task_id = state.max_task_id + 1;

    if (next_task_id < 1)
    {
        next_task_id = 1;
    }

    if (snapshot_loaded &&
        snapshot.next_task_id > (uint64_t)next_task_id)
    {
        next_task_id = (int)snapshot.next_task_id;
    }

    table->next_task_id = next_task_id;

    log_info("recovery_complete",
             "Recovered snapshot_lsn=%llu records=%d submitted=%d "
             "active=%zu retained=%zu next_task_id=%d",
             (unsigned long long)(snapshot_loaded ? snapshot.lsn : 0),
             records,
             state.submit_count,
             state.map.count,
             coordinator_history_count(&table->history),
             table->next_task_id);

    snapshot_data_free(&snapshot);
    recovery_state_free(&state);

    if (!recovery_take_snapshot(runtime, table, &runtime->wal))
    {
        log_warn("startup_snapshot_failed",
                 "Could not write the startup snapshot; continuing "
                 "with the existing WAL");
    }

    return 1;
}
