#ifndef LOG_H
#define LOG_H

typedef enum
{
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} LogLevel;

/*
 * Read IDR_LOG_LEVEL from the environment and configure the
 * default level. Idempotent: calling twice has no effect.
 * If the env var is unset or unrecognized, INFO is used.
 */
void log_init(void);

void log_set_level(LogLevel level);
LogLevel log_get_level(void);

void log_debug(const char *event, const char *fmt, ...);
void log_info (const char *event, const char *fmt, ...);
void log_warn (const char *event, const char *fmt, ...);
void log_error(const char *event, const char *fmt, ...);

#endif
