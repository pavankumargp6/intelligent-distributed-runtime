#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "wal.h"

#define WAL_LINE_MAX 8192

int wal_open_for_append(struct Wal *wal, const char *path)
{
    if (wal == NULL || path == NULL)
    {
        return 0;
    }

    wal->fd = -1;

    int fd = open(path,
                  O_WRONLY | O_CREAT | O_APPEND,
                  0644);

    if (fd < 0)
    {
        return 0;
    }

    wal->fd = fd;

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

int wal_append(struct Wal *wal, const char *line)
{
    if (wal == NULL || wal->fd < 0 || line == NULL)
    {
        return 0;
    }

    size_t line_length = strlen(line);

    if (line_length == 0 ||
        line_length > WAL_LINE_MAX - 2 ||
        memchr(line, '\n', line_length) != NULL)
    {
        return 0;
    }

    /*
     * Single write: line + '\n'. On a regular file this
     * is atomic up to PIPE_BUF under POSIX, and up to much
     * more on ext4/xfs for reasonable sizes.
     */
    char buffer[WAL_LINE_MAX];

    memcpy(buffer, line, line_length);
    buffer[line_length] = '\n';

    size_t total = line_length + 1;

    ssize_t written = 0;

    while ((size_t)written < total)
    {
        ssize_t n = write(wal->fd,
                          buffer + written,
                          total - (size_t)written);

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

        written += n;
    }

    if (fsync(wal->fd) != 0)
    {
        return 0;
    }

    return 1;
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

        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r'))
        {
            line[--len] = '\0';
        }

        if (len == 0)
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
