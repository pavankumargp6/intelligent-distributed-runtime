#ifndef WAL_H
#define WAL_H

/*
 * Append-only write-ahead log.
 *
 * One record per line, newline-terminated. No framing,
 * no length prefix — the file itself is the framing.
 * Commands and arguments must be escaped by the caller
 * (length-prefixed, as with the wire protocol) so that
 * embedded newlines cannot corrupt a record boundary.
 */

struct Wal
{
    int fd;
};

/*
 * Open the WAL for append. Creates the file if it does
 * not exist. Returns 1 on success, 0 on failure.
 */
int wal_open_for_append(struct Wal *wal, const char *path);

/*
 * Close the WAL. Safe to call on a zero-initialized struct.
 */
int wal_close(struct Wal *wal);

/*
 * Append one record. A newline is added automatically.
 * fsync is called before returning, so on success the
 * record is durable.
 *
 * Returns 1 on success, 0 on failure.
 */
int wal_append(struct Wal *wal, const char *line);

/*
 * Line callback. Return 0 to stop iteration, 1 to continue.
 */
typedef int (*wal_line_cb)(const char *line, void *user);

/*
 * Read every line from the WAL file at path and call cb on
 * each. Lines are NUL-terminated and do not include the
 * trailing newline.
 *
 * If the file does not exist, returns 0 with no callback
 * invocations (not an error).
 *
 * Returns the number of lines read on success, or -1 on
 * a read error.
 */
int wal_foreach_line(const char *path,
                     wal_line_cb cb,
                     void *user);

#endif
