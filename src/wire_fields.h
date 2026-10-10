#ifndef WIRE_FIELDS_H
#define WIRE_FIELDS_H

#include <stddef.h>

/*
 * Append a single '|' delimiter at offset.
 * Returns the new offset on success.
 * Returns 0 on failure.
 */
size_t wire_append_delimiter(char *buffer,
                             size_t buffer_size,
                             size_t offset);

/*
 * Append an opaque field as "<length>:<bytes>".
 * The value may contain '|', spaces, or newlines.
 * Returns the new offset on success.
 * Returns 0 on failure.
 */
size_t wire_append_field(char *buffer,
                         size_t buffer_size,
                         size_t offset,
                         const char *value);

/*
 * Append a plain decimal integer field.
 * Returns the new offset on success.
 * Returns 0 on failure.
 */
size_t wire_append_int(char *buffer,
                       size_t buffer_size,
                       size_t offset,
                       long value);

/*
 * Consume one '|' from *cursor.
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int wire_read_delimiter(const char **cursor);

/*
 * Consume one "<length>:<bytes>" field from *cursor.
 * The bytes are copied into out and NUL-terminated.
 * *cursor is advanced past the bytes (but not past a
 * following delimiter).
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int wire_read_field(const char **cursor,
                    char *out,
                    size_t out_size);

/*
 * Consume one plain decimal integer from *cursor.
 * Returns 1 on success.
 * Returns 0 on failure.
 */
int wire_read_int(const char **cursor,
                  long *out);

#endif
