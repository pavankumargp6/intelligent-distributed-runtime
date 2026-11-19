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
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "wal.h"
#include "log.h"

#define SEGMENT_MAGIC "IDRS"
#define RECORD_MAGIC "WLR2"

/*
 * Scale tests build wal.c with IDR_TEST_WAL_NO_SYNC so they can
 * generate millions of framed records without paying one fsync
 * per record. Production builds always fsync.
 */
#ifdef IDR_TEST_WAL_NO_SYNC
#define wal_fsync_record(fd) (1)
#else
#define wal_fsync_record(fd) (fsync(fd) == 0)
#endif

#define SEGMENT_DEFAULT_MAX_BYTES WAL_DEFAULT_SEGMENT_BYTES
#define SNAPSHOT_DEFAULT_EVERY WAL_DEFAULT_SNAPSHOT_EVERY
#define SNAPSHOT_DEFAULT_SECONDS WAL_DEFAULT_SNAPSHOT_SECONDS
#define MIN_FREE_BYTES_DEFAULT (16u * 1024u * 1024u)

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

uint32_t wal_crc32c(uint32_t seed, const void *data, size_t length)
{
    static uint32_t table[256];
    static int initialized = 0;

    if (!initialized)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t value = i;

            for (int bit = 0; bit < 8; bit++)
            {
                value = (value & 1u)
                            ? (value >> 1) ^ 0x82f63b78u
                            : value >> 1;
            }

            table[i] = value;
        }

        initialized = 1;
    }

    uint32_t crc = seed ^ 0xffffffffu;
    const unsigned char *bytes = data;

    for (size_t i = 0; i < length; i++)
    {
        crc = table[(crc ^ bytes[i]) & 0xffu] ^ (crc >> 8);
    }

    return crc ^ 0xffffffffu;
}

static uint64_t env_u64(const char *name, uint64_t fallback,
                        uint64_t minimum, uint64_t maximum)
{
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0')
    {
        return fallback;
    }

    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);

    if (end == value || (end != NULL && *end != '\0'))
    {
        return fallback;
    }

    if (parsed < minimum)
    {
        return minimum;
    }

    if (parsed > maximum)
    {
        return maximum;
    }

    return (uint64_t)parsed;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *bytes = buffer;
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

static ssize_t pread_full(int fd, void *buffer, size_t length, off_t offset)
{
    unsigned char *bytes = buffer;
    size_t done = 0;

    while (done < length)
    {
        ssize_t n = pread(fd,
                          bytes + done,
                          length - done,
                          offset + (off_t)done);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -1;
        }

        if (n == 0)
        {
            break;
        }

        done += (size_t)n;
    }

    return (ssize_t)done;
}

static int parse_segment_sequence(const char *path, uint64_t *sequence)
{
    const char *dot = strrchr(path, '.');

    if (dot == NULL || dot[1] == '\0')
    {
        return 0;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(dot + 1, &end, 10);

    if (errno != 0 || end == dot + 1 || end == NULL || *end != '\0')
    {
        return 0;
    }

    *sequence = (uint64_t)value;

    return 1;
}

int wal_segment_path(const char *base_path,
                     uint64_t sequence,
                     char *out,
                     size_t out_size)
{
    int n = snprintf(out, out_size,
                     "%s.seg.%020llu",
                     base_path,
                     (unsigned long long)sequence);

    return n > 0 && (size_t)n < out_size;
}

int wal_snapshot_path(const char *base_path,
                      uint64_t lsn,
                      char *out,
                      size_t out_size)
{
    int n = snprintf(out, out_size,
                     "%s.snap.%020llu.dat",
                     base_path,
                     (unsigned long long)lsn);

    return n > 0 && (size_t)n < out_size;
}

int wal_manifest_path(const char *base_path,
                      char *out,
                      size_t out_size)
{
    int n = snprintf(out, out_size,
                     "%s.manifest",
                     base_path);

    return n > 0 && (size_t)n < out_size;
}

int wal_fsync_directory(const char *path)
{
    char directory[WAL_PATH_MAX];

    int n = snprintf(directory, sizeof(directory), "%s", path);

    if (n <= 0 || (size_t)n >= sizeof(directory))
    {
        return 0;
    }

    char *slash = strrchr(directory, '/');

    if (slash == NULL)
    {
        snprintf(directory, sizeof(directory), ".");
    }
    else if (slash == directory)
    {
        slash[1] = '\0';
    }
    else
    {
        *slash = '\0';
    }

    int fd = open(directory, O_RDONLY | O_DIRECTORY);

    if (fd < 0)
    {
        return 0;
    }

    int result = 1;

    if (fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP)
    {
        result = 0;
    }

    close(fd);

    return result;
}

static int check_disk_space(const char *base_path)
{
    char directory[WAL_PATH_MAX];

    int n = snprintf(directory, sizeof(directory), "%s", base_path);

    if (n <= 0 || (size_t)n >= sizeof(directory))
    {
        return 0;
    }

    char *slash = strrchr(directory, '/');

    if (slash == NULL)
    {
        snprintf(directory, sizeof(directory), ".");
    }
    else if (slash == directory)
    {
        slash[1] = '\0';
    }
    else
    {
        *slash = '\0';
    }

    struct statvfs info;

    if (statvfs(directory, &info) != 0)
    {
        /*
         * Do not refuse to start just because the filesystem
         * does not implement statvfs. The subsequent writes
         * will surface a real error.
         */
        return 1;
    }

    uint64_t free_bytes = (uint64_t)info.f_bavail *
                          (uint64_t)info.f_frsize;

    uint64_t minimum = env_u64("IDR_MIN_FREE_BYTES",
                               MIN_FREE_BYTES_DEFAULT,
                               0,
                               UINT64_MAX);

    if (free_bytes < minimum)
    {
        log_error("wal_disk_low",
                  "Refusing to start: only %llu bytes free on the "
                  "WAL filesystem; IDR_MIN_FREE_BYTES requires %llu.",
                  (unsigned long long)free_bytes,
                  (unsigned long long)minimum);
        return 0;
    }

    return 1;
}

static void fill_segment_header(unsigned char *header,
                                uint64_t sequence,
                                time_t created)
{
    memcpy(header, SEGMENT_MAGIC, 4);
    put_u16_le(header + 4, WAL_FORMAT_VERSION);
    put_u16_le(header + 6, WAL_SEGMENT_HEADER_SIZE);
    put_u64_le(header + 8, sequence);
    put_u64_le(header + 16, (uint64_t)created);

    uint32_t crc = wal_crc32c(0, header, 20);

    put_u32_le(header + 20, crc);
}

static int write_segment_header(int fd, uint64_t sequence)
{
    unsigned char header[WAL_SEGMENT_HEADER_SIZE];

    fill_segment_header(header, sequence, time(NULL));

    if (lseek(fd, 0, SEEK_SET) < 0)
    {
        return 0;
    }

    if (!write_all(fd, header, sizeof(header)))
    {
        return 0;
    }

    return fsync(fd) == 0;
}

static int validate_segment_header(int fd,
                                   uint64_t expected_sequence,
                                   uint64_t file_size)
{
    unsigned char header[WAL_SEGMENT_HEADER_SIZE];

    if (file_size < WAL_SEGMENT_HEADER_SIZE)
    {
        return 0;
    }

    ssize_t n = pread_full(fd, header, sizeof(header), 0);

    if (n != (ssize_t)sizeof(header))
    {
        return 0;
    }

    if (memcmp(header, SEGMENT_MAGIC, 4) != 0)
    {
        return 0;
    }

    if (get_u16_le(header + 4) != WAL_FORMAT_VERSION)
    {
        return 0;
    }

    if (get_u16_le(header + 6) != WAL_SEGMENT_HEADER_SIZE)
    {
        return 0;
    }

    if (get_u64_le(header + 8) != expected_sequence)
    {
        return 0;
    }

    uint32_t stored_crc = get_u32_le(header + 20);
    uint32_t computed_crc = wal_crc32c(0, header, 20);

    return stored_crc == computed_crc;
}

static int create_segment(const char *path,
                          uint64_t sequence,
                          int *fd_out)
{
    int fd = open(path,
                  O_RDWR | O_CREAT | O_TRUNC,
                  0644);

    if (fd < 0)
    {
        log_error("wal_segment_open_failed",
                  "Could not create segment %s: %s",
                  path, strerror(errno));
        return 0;
    }

    if (!write_segment_header(fd, sequence))
    {
        log_error("wal_segment_header_failed",
                  "Could not write segment header for %s: %s",
                  path, strerror(errno));
        close(fd);
        unlink(path);
        return 0;
    }

    if (!wal_fsync_directory(path))
    {
        log_warn("wal_directory_fsync_failed",
                 "Could not fsync the directory containing %s",
                 path);
    }

    *fd_out = fd;

    return 1;
}

/*
 * Scan one segment from the beginning. Returns:
 *   1  clean
 *   0  torn tail (truncated to the last valid record)
 *  -1  corruption
 */
static int scan_segment(int fd,
                        uint64_t file_size,
                        const char *path,
                        uint64_t *last_lsn_out,
                        uint64_t *last_offset_out)
{
    uint64_t offset = WAL_SEGMENT_HEADER_SIZE;
    uint64_t last_lsn = 0;
    uint64_t last_offset = WAL_SEGMENT_HEADER_SIZE;
    int torn = 0;

    while (offset < file_size)
    {
        unsigned char header[WAL_RECORD_HEADER_SIZE];

        uint64_t remaining = file_size - offset;

        if (remaining < WAL_RECORD_HEADER_SIZE)
        {
            torn = 1;
            break;
        }

        ssize_t n = pread_full(fd, header, sizeof(header), (off_t)offset);

        if (n != (ssize_t)sizeof(header))
        {
            torn = 1;
            break;
        }

        if (memcmp(header, RECORD_MAGIC, 4) != 0 ||
            get_u16_le(header + 4) != WAL_FORMAT_VERSION)
        {
            log_error("wal_record_header_invalid",
                      "Invalid record header in %s at offset %llu",
                      path, (unsigned long long)offset);
            return -1;
        }

        uint8_t type = header[6];
        uint32_t payload_length = get_u32_le(header + 16);
        uint64_t lsn = get_u64_le(header + 8);

        if (type < WAL_RECORD_SUBMIT || type > WAL_RECORD_RESULT ||
            payload_length == 0 ||
            payload_length > WAL_LINE_MAX - 2 ||
            lsn == 0)
        {
            log_error("wal_record_invalid",
                      "Invalid record metadata in %s at offset %llu",
                      path, (unsigned long long)offset);
            return -1;
        }

        uint64_t record_size = WAL_RECORD_HEADER_SIZE +
                               (uint64_t)payload_length + 4;

        if (record_size > remaining)
        {
            torn = 1;
            break;
        }

        unsigned char payload_and_crc[WAL_LINE_MAX + 4];

        n = pread_full(fd,
                       payload_and_crc,
                       (size_t)payload_length + 4,
                       (off_t)(offset + WAL_RECORD_HEADER_SIZE));

        if (n != (ssize_t)(payload_length + 4))
        {
            torn = 1;
            break;
        }

        uint32_t stored_crc =
            get_u32_le(payload_and_crc + payload_length);

        uint32_t computed_crc = wal_crc32c(0, header,
                                           WAL_RECORD_HEADER_SIZE);

        computed_crc = wal_crc32c(computed_crc,
                                  payload_and_crc,
                                  payload_length);

        if (stored_crc != computed_crc)
        {
            if (offset + record_size == file_size)
            {
                torn = 1;
                break;
            }

            log_error("wal_record_crc_invalid",
                      "CRC mismatch in %s at offset %llu",
                      path, (unsigned long long)offset);
            return -1;
        }

        if (last_lsn != 0 && lsn <= last_lsn)
        {
            log_error("wal_lsn_not_increasing",
                      "Non-increasing LSN in %s at offset %llu",
                      path, (unsigned long long)offset);
            return -1;
        }

        last_lsn = lsn;
        offset += record_size;
        last_offset = offset;
    }

    *last_lsn_out = last_lsn;
    *last_offset_out = last_offset;

    if (!torn)
    {
        return 1;
    }

    if (ftruncate(fd, (off_t)last_offset) != 0 ||
        fsync(fd) != 0)
    {
        log_error("wal_torn_truncate_failed",
                  "Could not truncate torn tail in %s: %s",
                  path, strerror(errno));
        return -1;
    }

    log_warn("wal_torn_tail_truncated",
             "Truncated torn final record in %s at offset %llu",
             path, (unsigned long long)last_offset);

    return 0;
}

int wal_find_active_segment(const char *base_path,
                            uint64_t *sequence)
{
    if (base_path == NULL || sequence == NULL)
    {
        return -1;
    }

    char pattern[WAL_PATH_MAX];

    int n = snprintf(pattern, sizeof(pattern),
                     "%s.seg.*", base_path);

    if (n <= 0 || (size_t)n >= sizeof(pattern))
    {
        return -1;
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
        return -1;
    }

    int found = 0;
    uint64_t highest = 0;

    for (size_t i = 0; i < matches.gl_pathc; i++)
    {
        uint64_t value = 0;

        if (!parse_segment_sequence(matches.gl_pathv[i], &value))
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

    if (!found)
    {
        return 0;
    }

    *sequence = highest;

    return 1;
}

int wal_open_for_append(struct Wal *wal, const char *base_path)
{
    if (wal == NULL || base_path == NULL ||
        base_path[0] == '\0')
    {
        return 0;
    }

    size_t length = strlen(base_path);

    if (length >= sizeof(wal->base_path))
    {
        log_error("wal_path_too_long",
                  "WAL base path is too long (max %zu bytes)",
                  sizeof(wal->base_path) - 1);
        return 0;
    }

    memset(wal, 0, sizeof(*wal));
    wal->fd = -1;
    memcpy(wal->base_path, base_path, length + 1);

    if (!check_disk_space(base_path))
    {
        return 0;
    }

    wal->segment_max_bytes = env_u64("IDR_SEGMENT_BYTES",
                                     SEGMENT_DEFAULT_MAX_BYTES,
                                     4096,
                                     1024ull * 1024ull * 1024ull);

    uint64_t sequence = 0;
    int have_segment = wal_find_active_segment(base_path, &sequence);

    if (have_segment < 0)
    {
        log_error("wal_segment_list_failed",
                  "Could not list WAL segments for %s",
                  base_path);
        return 0;
    }

    int fd = -1;

    if (!have_segment)
    {
        sequence = 1;

        char path[WAL_PATH_MAX];

        if (!wal_segment_path(base_path, sequence,
                              path, sizeof(path)) ||
            !create_segment(path, sequence, &fd))
        {
            return 0;
        }

        wal->segment_bytes = WAL_SEGMENT_HEADER_SIZE;
        wal->next_lsn = 1;
    }
    else
    {
        char path[WAL_PATH_MAX];

        if (!wal_segment_path(base_path, sequence,
                              path, sizeof(path)))
        {
            return 0;
        }

        fd = open(path, O_RDWR | O_APPEND);

        if (fd < 0)
        {
            log_error("wal_segment_open_failed",
                      "Could not open segment %s: %s",
                      path, strerror(errno));
            return 0;
        }

        struct stat info;

        if (fstat(fd, &info) != 0)
        {
            log_error("wal_segment_stat_failed",
                      "Could not stat segment %s: %s",
                      path, strerror(errno));
            close(fd);
            return 0;
        }

        uint64_t file_size = (uint64_t)info.st_size;

        if (file_size < WAL_SEGMENT_HEADER_SIZE)
        {
            if (ftruncate(fd, 0) != 0 ||
                !write_segment_header(fd, sequence))
            {
                log_error("wal_segment_repair_failed",
                          "Could not initialize empty segment %s",
                          path);
                close(fd);
                return 0;
            }

            file_size = WAL_SEGMENT_HEADER_SIZE;
        }
        else if (!validate_segment_header(fd, sequence, file_size))
        {
            log_error("wal_segment_header_invalid",
                      "Segment header is invalid or has an unknown "
                      "version: %s",
                      path);
            close(fd);
            return 0;
        }

        uint64_t last_lsn = 0;
        uint64_t last_offset = WAL_SEGMENT_HEADER_SIZE;

        int scan = scan_segment(fd, file_size, path,
                                &last_lsn, &last_offset);

        if (scan < 0)
        {
            close(fd);
            return 0;
        }

        if (lseek(fd, 0, SEEK_END) < 0)
        {
            close(fd);
            return 0;
        }

        struct stat after;

        if (fstat(fd, &after) != 0)
        {
            close(fd);
            return 0;
        }

        wal->segment_bytes = (uint64_t)after.st_size;
        wal->next_lsn = last_lsn + 1;

        if (wal->next_lsn == 1 && sequence > 1)
        {
            /*
             * The active segment is empty (for example, the
             * process crashed immediately after rotation).
             * Recovery corrects next_lsn from the replay
             * result; a direct wal_open_for_append caller
             * starts from 1, which is safe for a fresh log.
             */
            wal->next_lsn = 1;
        }
    }

    wal->fd = fd;
    wal->active_seq = sequence;
    wal->total_records = 0;
    wal->records_since_snapshot = 0;
    wal->last_snapshot_time = time(NULL);

    return 1;
}

int wal_close(struct Wal *wal)
{
    if (wal == NULL)
    {
        return 0;
    }

    if (wal->fd >= 0)
    {
        close(wal->fd);
        wal->fd = -1;
    }

    return 1;
}

int wal_rotate(struct Wal *wal)
{
    if (wal == NULL || wal->fd < 0)
    {
        return 0;
    }

    uint64_t next_sequence = wal->active_seq + 1;

    char path[WAL_PATH_MAX];

    if (!wal_segment_path(wal->base_path,
                          next_sequence,
                          path,
                          sizeof(path)))
    {
        return 0;
    }

    int new_fd = -1;

    if (!create_segment(path, next_sequence, &new_fd))
    {
        return 0;
    }

    idr_test_crash_point("segment_after_rotate");

    int old_fd = wal->fd;

    wal->fd = new_fd;
    wal->active_seq = next_sequence;
    wal->segment_bytes = WAL_SEGMENT_HEADER_SIZE;

    close(old_fd);

    return 1;
}

static int infer_record_type(const char *line,
                             enum WalRecordType *type)
{
    if (strncmp(line, "SUBMIT|", 7) == 0)
    {
        *type = WAL_RECORD_SUBMIT;
        return 1;
    }

    if (strncmp(line, "DISPATCH|", 9) == 0)
    {
        *type = WAL_RECORD_DISPATCH;
        return 1;
    }

    if (strncmp(line, "ACCEPT|", 7) == 0)
    {
        *type = WAL_RECORD_ACCEPT;
        return 1;
    }

    if (strncmp(line, "RESULT|", 7) == 0)
    {
        *type = WAL_RECORD_RESULT;
        return 1;
    }

    return 0;
}

int wal_append(struct Wal *wal, const char *line)
{
    if (line == NULL)
    {
        return 0;
    }

    enum WalRecordType type;

    if (!infer_record_type(line, &type))
    {
        return 0;
    }

    return wal_append_typed(wal, type, line);
}

int wal_append_typed(struct Wal *wal,
                     enum WalRecordType type,
                     const char *payload)
{
    if (wal == NULL || wal->fd < 0 || payload == NULL)
    {
        return 0;
    }

    size_t payload_length = strlen(payload);

    if (payload_length == 0 ||
        payload_length > WAL_LINE_MAX - 2 ||
        memchr(payload, '\n', payload_length) != NULL)
    {
        return 0;
    }

    if (type < WAL_RECORD_SUBMIT || type > WAL_RECORD_RESULT)
    {
        return 0;
    }

    if (wal->segment_bytes >= wal->segment_max_bytes)
    {
        if (!wal_rotate(wal))
        {
            log_error("wal_rotate_failed",
                      "Could not rotate WAL segment; append rejected");
            return 0;
        }
    }

    unsigned char buffer[WAL_RECORD_OVERHEAD + WAL_LINE_MAX];

    memcpy(buffer, RECORD_MAGIC, 4);
    put_u16_le(buffer + 4, WAL_FORMAT_VERSION);
    buffer[6] = (unsigned char)type;
    buffer[7] = 0;
    put_u64_le(buffer + 8, wal->next_lsn);
    put_u32_le(buffer + 16, (uint32_t)payload_length);

    memcpy(buffer + WAL_RECORD_HEADER_SIZE,
           payload,
           payload_length);

    uint32_t crc = wal_crc32c(0, buffer, WAL_RECORD_HEADER_SIZE);
    crc = wal_crc32c(crc, payload, payload_length);

    put_u32_le(buffer + WAL_RECORD_HEADER_SIZE + payload_length, crc);

    size_t total = WAL_RECORD_HEADER_SIZE + payload_length + 4;

    if (!write_all(wal->fd, buffer, total))
    {
        log_error("wal_append_write_failed",
                  "Write to WAL segment %llu failed: %s",
                  (unsigned long long)wal->active_seq,
                  strerror(errno));
        close(wal->fd);
        wal->fd = -1;
        return 0;
    }

    idr_test_crash_point("wal_after_write");

    if (!wal_fsync_record(wal->fd))
    {
        log_error("wal_append_fsync_failed",
                  "fsync of WAL segment %llu failed: %s",
                  (unsigned long long)wal->active_seq,
                  strerror(errno));
        close(wal->fd);
        wal->fd = -1;
        return 0;
    }

    idr_test_crash_point("wal_after_fsync");

    wal->next_lsn++;
    wal->segment_bytes += total;
    wal->records_since_snapshot++;
    wal->total_records++;

    return 1;
}

int wal_should_snapshot(const struct Wal *wal)
{
    if (wal == NULL || wal->fd < 0)
    {
        return 0;
    }

    uint64_t every = env_u64("IDR_SNAPSHOT_EVERY",
                             SNAPSHOT_DEFAULT_EVERY,
                             1,
                             UINT64_MAX);

    if (wal->records_since_snapshot >= every)
    {
        return 1;
    }

    if (wal->segment_bytes >= wal->segment_max_bytes)
    {
        return 1;
    }

    uint64_t seconds = env_u64("IDR_SNAPSHOT_SECONDS",
                               SNAPSHOT_DEFAULT_SECONDS,
                               1,
                               UINT64_MAX);

    if (wal->records_since_snapshot > 0 &&
        wal->last_snapshot_time > 0 &&
        difftime(time(NULL), wal->last_snapshot_time) >=
            (double)seconds)
    {
        return 1;
    }

    return 0;
}

static int segment_header_ok(int fd,
                             uint64_t expected_sequence,
                             uint64_t file_size)
{
    return validate_segment_header(fd, expected_sequence, file_size);
}

int wal_validate_segment_chain(const char *base_path,
                               uint64_t boundary_seq)
{
    if (base_path == NULL)
    {
        return 0;
    }

    char pattern[WAL_PATH_MAX];

    int n = snprintf(pattern, sizeof(pattern), "%s.seg.*", base_path);

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

        log_error("wal_segment_missing",
                  "Snapshot covers through segment %llu but no segments "
                  "exist for %s",
                  (unsigned long long)boundary_seq,
                  base_path);
        return 0;
    }

    if (rc != 0)
    {
        globfree(&matches);
        return 0;
    }

    uint64_t previous = 0;
    uint64_t first = 0;
    uint64_t last = 0;
    int have_segment = 0;

    for (size_t i = 0; i < matches.gl_pathc; i++)
    {
        uint64_t sequence = 0;

        if (!parse_segment_sequence(matches.gl_pathv[i], &sequence))
        {
            continue;
        }

        if (!have_segment)
        {
            first = sequence;
        }
        else if (sequence != previous + 1)
        {
            log_error("wal_segment_gap",
                      "Missing WAL segment between sequence %llu and "
                      "%llu for %s; refusing to recover incomplete state",
                      (unsigned long long)previous,
                      (unsigned long long)sequence,
                      base_path);
            globfree(&matches);
            return 0;
        }

        previous = sequence;
        last = sequence;
        have_segment = 1;
    }

    globfree(&matches);

    if (!have_segment)
    {
        log_error("wal_segment_missing",
                  "Snapshot covers through segment %llu but none could "
                  "be parsed for %s",
                  (unsigned long long)boundary_seq,
                  base_path);
        return 0;
    }

    /*
     * boundary_seq is the last segment fully covered by the
     * snapshot; the rotation that follows it creates
     * boundary_seq + 1. Segments below that floor may be deleted
     * safely, but the floor itself must still be present: without
     * it, records appended after the snapshot could be missing.
     */
    uint64_t required = boundary_seq + 1;

    if (first > required || last < required)
    {
        log_error("wal_segment_gap",
                  "Snapshot covers through segment %llu, but the "
                  "available segment range is %llu-%llu for %s; "
                  "refusing to recover incomplete state",
                  (unsigned long long)boundary_seq,
                  (unsigned long long)first,
                  (unsigned long long)last,
                  base_path);
        return 0;
    }

    return 1;
}

int wal_foreach_record(const char *base_path,
                       uint64_t after_lsn,
                       wal_record_cb cb,
                       void *user)
{
    if (base_path == NULL || cb == NULL)
    {
        return -1;
    }

    char pattern[WAL_PATH_MAX];

    int n = snprintf(pattern, sizeof(pattern),
                     "%s.seg.*", base_path);

    if (n <= 0 || (size_t)n >= sizeof(pattern))
    {
        return -1;
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
        return -1;
    }

    int records = 0;
    uint64_t last_lsn = 0;
    int stopped = 0;

    for (size_t file_index = 0;
         file_index < matches.gl_pathc && !stopped;
         file_index++)
    {
        const char *path = matches.gl_pathv[file_index];
        uint64_t expected_sequence = 0;

        if (!parse_segment_sequence(path, &expected_sequence))
        {
            continue;
        }

        int fd = open(path, O_RDONLY);

        if (fd < 0)
        {
            globfree(&matches);
            return -1;
        }

        struct stat info;

        if (fstat(fd, &info) != 0 ||
            info.st_size < (off_t)WAL_SEGMENT_HEADER_SIZE ||
            !segment_header_ok(fd, expected_sequence,
                               (uint64_t)info.st_size))
        {
            log_error("wal_segment_invalid",
                      "Segment is invalid or has an unknown version: %s",
                      path);
            close(fd);
            globfree(&matches);
            return -1;
        }

        uint64_t offset = WAL_SEGMENT_HEADER_SIZE;
        uint64_t file_size = (uint64_t)info.st_size;
        int last_segment = (file_index + 1 == matches.gl_pathc);
        int torn = 0;

        while (offset < file_size && !stopped)
        {
            unsigned char header[WAL_RECORD_HEADER_SIZE];
            uint64_t remaining = file_size - offset;

            if (remaining < WAL_RECORD_HEADER_SIZE)
            {
                torn = 1;
                break;
            }

            ssize_t got = pread_full(fd, header, sizeof(header),
                                     (off_t)offset);

            if (got != (ssize_t)sizeof(header))
            {
                torn = 1;
                break;
            }

            if (memcmp(header, RECORD_MAGIC, 4) != 0 ||
                get_u16_le(header + 4) != WAL_FORMAT_VERSION)
            {
                log_error("wal_record_header_invalid",
                          "Invalid record header in %s at offset %llu",
                          path, (unsigned long long)offset);
                close(fd);
                globfree(&matches);
                return -1;
            }

            uint8_t type = header[6];
            uint32_t payload_length = get_u32_le(header + 16);
            uint64_t lsn = get_u64_le(header + 8);

            if (type < WAL_RECORD_SUBMIT ||
                type > WAL_RECORD_RESULT ||
                payload_length == 0 ||
                payload_length > WAL_LINE_MAX - 2 ||
                lsn == 0)
            {
                log_error("wal_record_invalid",
                          "Invalid record metadata in %s at offset %llu",
                          path, (unsigned long long)offset);
                close(fd);
                globfree(&matches);
                return -1;
            }

            uint64_t record_size = WAL_RECORD_HEADER_SIZE +
                                   (uint64_t)payload_length + 4;

            if (record_size > remaining)
            {
                torn = 1;
                break;
            }

            unsigned char payload_and_crc[WAL_LINE_MAX + 4];

            got = pread_full(fd,
                             payload_and_crc,
                             (size_t)payload_length + 4,
                             (off_t)(offset + WAL_RECORD_HEADER_SIZE));

            if (got != (ssize_t)(payload_length + 4))
            {
                torn = 1;
                break;
            }

            uint32_t stored_crc =
                get_u32_le(payload_and_crc + payload_length);

            uint32_t computed_crc = wal_crc32c(0, header,
                                               WAL_RECORD_HEADER_SIZE);

            computed_crc = wal_crc32c(computed_crc,
                                      payload_and_crc,
                                      payload_length);

            if (stored_crc != computed_crc)
            {
                if (offset + record_size == file_size)
                {
                    torn = 1;
                    break;
                }

                log_error("wal_record_crc_invalid",
                          "CRC mismatch in %s at offset %llu",
                          path, (unsigned long long)offset);
                close(fd);
                globfree(&matches);
                return -1;
            }

            if (last_lsn != 0 && lsn <= last_lsn)
            {
                log_error("wal_lsn_not_increasing",
                          "Non-increasing LSN in %s at offset %llu",
                          path, (unsigned long long)offset);
                close(fd);
                globfree(&matches);
                return -1;
            }

            last_lsn = lsn;
            offset += record_size;
            records++;

            if (lsn > after_lsn)
            {
                char payload[WAL_LINE_MAX];

                memcpy(payload, payload_and_crc, payload_length);
                payload[payload_length] = '\0';

                if (cb((enum WalRecordType)type,
                       lsn,
                       payload,
                       user) == 0)
                {
                    stopped = 1;
                }
            }
        }

        if (torn)
        {
            if (!last_segment)
            {
                log_error("wal_midfile_torn",
                          "Torn record before the final segment in %s",
                          path);
                close(fd);
                globfree(&matches);
                return -1;
            }

            uint64_t truncate_at = offset;

            /*
             * The segment was opened read-only for replay; reopen
             * it read-write so the torn tail can actually be cut.
             */
            close(fd);
            fd = open(path, O_RDWR);

            if (fd < 0)
            {
                log_error("wal_torn_truncate_failed",
                          "Could not reopen %s read-write: %s",
                          path, strerror(errno));
                globfree(&matches);
                return -1;
            }

            if (ftruncate(fd, (off_t)truncate_at) != 0 ||
                fsync(fd) != 0)
            {
                log_error("wal_torn_truncate_failed",
                          "Could not truncate torn tail in %s: %s",
                          path, strerror(errno));
                close(fd);
                globfree(&matches);
                return -1;
            }

            log_warn("wal_torn_tail_truncated",
                     "Truncated torn final record in %s at offset %llu",
                     path, (unsigned long long)truncate_at);
        }

        close(fd);
    }

    globfree(&matches);

    return records;
}

int wal_remove_segments_before(const char *base_path,
                               uint64_t keep_from)
{
    if (base_path == NULL)
    {
        return -1;
    }

    char pattern[WAL_PATH_MAX];

    int n = snprintf(pattern, sizeof(pattern),
                     "%s.seg.*", base_path);

    if (n <= 0 || (size_t)n >= sizeof(pattern))
    {
        return -1;
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
        return -1;
    }

    int removed = 0;

    for (size_t i = 0; i < matches.gl_pathc; i++)
    {
        uint64_t sequence = 0;

        if (!parse_segment_sequence(matches.gl_pathv[i], &sequence))
        {
            continue;
        }

        if (sequence >= keep_from)
        {
            continue;
        }

        idr_test_crash_point("segment_before_unlink");

        if (unlink(matches.gl_pathv[i]) != 0)
        {
            log_error("wal_segment_unlink_failed",
                      "Could not remove segment %s: %s",
                      matches.gl_pathv[i], strerror(errno));
            globfree(&matches);
            return -1;
        }

        removed++;
    }

    globfree(&matches);

    if (removed > 0)
    {
        char probe[WAL_PATH_MAX];

        if (wal_segment_path(base_path, 0, probe, sizeof(probe)))
        {
            (void)wal_fsync_directory(probe);
        }

        log_info("wal_segments_truncated",
                 "Removed %d WAL segment(s) below sequence %llu",
                 removed, (unsigned long long)keep_from);
    }

    return removed;
}

int wal_parse_record_type(const char *payload,
                          enum WalRecordType *type)
{
    if (payload == NULL || type == NULL)
    {
        return 0;
    }

    return infer_record_type(payload, type);
}

void idr_test_crash_point(const char *name)
{
    const char *wanted = getenv("IDR_TEST_CRASH_AT");

    if (wanted == NULL || name == NULL ||
        strcmp(wanted, name) != 0)
    {
        return;
    }

    fprintf(stderr, "IDR_TEST_CRASH_AT=%s\n", name);
    fflush(NULL);

    _exit(99);
}

int wal_foreach_line(const char *path,
                     wal_line_cb cb,
                     void *user)
{
    if (path == NULL || cb == NULL)
    {
        return -1;
    }

    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        if (errno == ENOENT)
        {
            return 0;
        }
        return -1;
    }

    char line[WAL_LINE_MAX];
    int lines = 0;
    int stopped = 0;

    while (fgets(line, sizeof(line), file) != NULL)
    {
        size_t len = strlen(line);
        int truncated = 0;

        if (len > 0 && line[len - 1] != '\n')
        {
            if (!feof(file))
            {
                int c;

                while ((c = fgetc(file)) != EOF && c != '\n')
                {
                    /* discard */
                }
            }

            truncated = 1;
        }

        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
            line[--len] = '\0';
        }

        if (truncated || len == 0)
        {
            continue;
        }

        lines++;

        if (cb(line, user) == 0)
        {
            stopped = 1;
            break;
        }
    }

    if (ferror(file) && !stopped)
    {
        fclose(file);
        return -1;
    }

    fclose(file);

    return lines;
}
