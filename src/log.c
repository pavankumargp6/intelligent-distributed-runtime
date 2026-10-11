#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "log.h"

static LogLevel g_level = LOG_INFO;
static int g_initialized = 0;

void log_set_level(LogLevel level)
{
    g_level = level;
}

LogLevel log_get_level(void)
{
    return g_level;
}

void log_init(void)
{
    if (g_initialized)
    {
        return;
    }
    g_initialized = 1;

    const char *env = getenv("IDR_LOG_LEVEL");

    if (env == NULL)
    {
        return;
    }

    if (strcmp(env, "debug") == 0)      g_level = LOG_DEBUG;
    else if (strcmp(env, "info") == 0)  g_level = LOG_INFO;
    else if (strcmp(env, "warn") == 0)  g_level = LOG_WARN;
    else if (strcmp(env, "error") == 0) g_level = LOG_ERROR;
}

static const char *level_name(LogLevel level)
{
    switch (level)
    {
        case LOG_DEBUG: return "debug";
        case LOG_INFO:  return "info";
        case LOG_WARN:  return "warn";
        case LOG_ERROR: return "error";
        default:        return "unknown";
    }
}

/*
 * Copy src into dst, JSON-escaping quotes, backslashes, and
 * control characters. dst is always NUL-terminated if dst_size
 * is greater than zero.
 */
static void json_escape(const char *src, char *dst, size_t dst_size)
{
    if (dst_size == 0)
    {
        return;
    }

    size_t j = 0;

    for (size_t i = 0; src != NULL && src[i] != '\0'; i++)
    {
        unsigned char c = (unsigned char)src[i];

        if (c == '"' || c == '\\')
        {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = (char)c;
        }
        else if (c == '\n')
        {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'n';
        }
        else if (c == '\r')
        {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 'r';
        }
        else if (c == '\t')
        {
            if (j + 2 >= dst_size) break;
            dst[j++] = '\\';
            dst[j++] = 't';
        }
        else if (c < 0x20)
        {
            if (j + 6 >= dst_size) break;
            snprintf(dst + j, 7, "\\u%04x", c);
            j += 6;
        }
        else
        {
            dst[j++] = (char)c;
        }
    }

    dst[j] = '\0';
}

static void log_emit(LogLevel level,
                     const char *event,
                     const char *fmt,
                     va_list ap)
{
    if (!g_initialized)
    {
        log_init();
    }

    if (level < g_level)
    {
        return;
    }

    char raw_msg[768];
    vsnprintf(raw_msg, sizeof(raw_msg), fmt, ap);

    char escaped_msg[1024];
    json_escape(raw_msg, escaped_msg, sizeof(escaped_msg));

    char escaped_event[128];
    json_escape(event ? event : "", escaped_event, sizeof(escaped_event));

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_buf;
    gmtime_r(&ts.tv_sec, &tm_buf);

    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    flockfile(stdout);
    fprintf(stdout,
            "{\"ts\":\"%s.%03ldZ\",\"level\":\"%s\","
            "\"event\":\"%s\",\"msg\":\"%s\"}\n",
            timestamp,
            ts.tv_nsec / 1000000L,
            level_name(level),
            escaped_event,
            escaped_msg);
    fflush(stdout);
    funlockfile(stdout);
}

void log_debug(const char *event, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_emit(LOG_DEBUG, event, fmt, ap);
    va_end(ap);
}

void log_info(const char *event, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_emit(LOG_INFO, event, fmt, ap);
    va_end(ap);
}

void log_warn(const char *event, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_emit(LOG_WARN, event, fmt, ap);
    va_end(ap);
}

void log_error(const char *event, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_emit(LOG_ERROR, event, fmt, ap);
    va_end(ap);
}
