#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h> 
#include "wire_fields.h"

size_t wire_append_delimiter(char *buffer,
                             size_t buffer_size,
                             size_t offset)
{
    if (buffer == NULL || offset + 1 >= buffer_size)
    {
        return 0;
    }

    buffer[offset] = '|';
    buffer[offset + 1] = '\0';

    return offset + 1;
}

size_t wire_append_field(char *buffer,
                         size_t buffer_size,
                         size_t offset,
                         const char *value)
{
    if (buffer == NULL || value == NULL)
    {
        return 0;
    }

    size_t value_length = strlen(value);

    char prefix[32];
    int prefix_length = snprintf(prefix,
                                 sizeof(prefix),
                                 "%zu:",
                                 value_length);

    if (prefix_length <= 0 ||
        (size_t)prefix_length >= sizeof(prefix))
    {
        return 0;
    }

    size_t needed =
        offset + (size_t)prefix_length + value_length + 1;

    if (needed > buffer_size)
    {
        return 0;
    }

    memcpy(buffer + offset, prefix, (size_t)prefix_length);
    memcpy(buffer + offset + prefix_length,
           value,
           value_length);

    buffer[offset + prefix_length + value_length] = '\0';

    return offset + prefix_length + value_length;
}

size_t wire_append_int(char *buffer,
                       size_t buffer_size,
                       size_t offset,
                       long value)
{
    if (buffer == NULL)
    {
        return 0;
    }

    char digits[32];
    int digits_length = snprintf(digits,
                                 sizeof(digits),
                                 "%ld",
                                 value);

    if (digits_length <= 0 ||
        (size_t)digits_length >= sizeof(digits))
    {
        return 0;
    }

    if (offset + (size_t)digits_length + 1 > buffer_size)
    {
        return 0;
    }

    memcpy(buffer + offset, digits, (size_t)digits_length);
    buffer[offset + digits_length] = '\0';

    return offset + digits_length;
}

int wire_read_delimiter(const char **cursor)
{
    if (cursor == NULL || *cursor == NULL)
    {
        return 0;
    }

    if (**cursor != '|')
    {
        return 0;
    }

    (*cursor)++;

    return 1;
}

int wire_read_field(const char **cursor,
                    char *out,
                    size_t out_size)
{
    if (cursor == NULL || *cursor == NULL ||
        out == NULL || out_size == 0)
    {
        return 0;
    }

    const char *p = *cursor;
    char *endptr = NULL;

    long length = strtol(p, &endptr, 10);

    if (endptr == p || *endptr != ':' || length < 0)
    {
        return 0;
    }

    if ((size_t)length >= out_size)
    {
        return 0;
    }

    p = endptr + 1;

    size_t i;
    for (i = 0; i < (size_t)length; i++)
    {
        if (p[i] == '\0')
        {
            return 0;
        }
    }

    memcpy(out, p, (size_t)length);
    out[length] = '\0';

    *cursor = p + length;

    return 1;
}

int wire_read_int(const char **cursor, long *out)
{
    if (cursor == NULL || *cursor == NULL || out == NULL)
    {
        return 0;
    }

    const char *p = *cursor;
    char *endptr = NULL;

    errno = 0;
    long value = strtol(p, &endptr, 10);

    if (endptr == p || errno == ERANGE)
    {
        return 0;
    }

    *out = value;
    *cursor = endptr;

    return 1;
}
