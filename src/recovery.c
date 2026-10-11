#include <stdio.h>
#include <string.h>

#include "recovery.h"
#include "wal.h"
#include "wire_fields.h"
#include "log.h"

/*
 * Bounded recovery buffer. The WAL grows forever until we add
 * compaction (later phase), so cap what recovery will hold in
 * memory. If exceeded, we log a warning and stop collecting;
 * older records are not replayed.
 */
#define RECOVERY_MAX_TASKS 4096

struct RecoveredSubmit
{
    int task_id;
    struct Task task;
};

struct RecoveredResult
{
    int task_id;
    int worker_id;
    int exit_status;
    int state;
};

struct RecoveryContext
{
    struct RecoveredSubmit submits[RECOVERY_MAX_TASKS];
    int submit_count;

    struct RecoveredResult results[RECOVERY_MAX_TASKS];
    int result_count;

    int max_task_id;
    int overflow;
};

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

    if (!wire_read_int(&cursor, &task_id) || task_id <= 0)
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

    *task_id_out = (int)task_id;
    *task_out = task;

    return 1;
}

static int parse_result_record(const char *line,
                               int *task_id,
                               int *worker_id,
                               int *exit_status,
                               int *state)
{
    return sscanf(line,
                  "RESULT|%d|%d|%d|%d",
                  task_id,
                  worker_id,
                  exit_status,
                  state) == 4;
}

static int recovery_line_cb(const char *line, void *user)
{
    struct RecoveryContext *ctx = user;

    if (strncmp(line, "SUBMIT|", 7) == 0)
    {
        int task_id = 0;
        struct Task task;

        if (!parse_submit_record(line, &task_id, &task))
        {
            log_warn("wal_submit_invalid",
                     "Skipping malformed SUBMIT record: %s",
                     line);
            return 1;
        }

        if (ctx->submit_count >= RECOVERY_MAX_TASKS)
        {
            ctx->overflow = 1;
            return 1;
        }

        ctx->submits[ctx->submit_count].task_id = task_id;
        ctx->submits[ctx->submit_count].task = task;
        ctx->submit_count++;

        if (task_id > ctx->max_task_id)
        {
            ctx->max_task_id = task_id;
        }
    }
    else if (strncmp(line, "RESULT|", 7) == 0)
    {
        int task_id = 0;
        int worker_id = 0;
        int exit_status = 0;
        int state = 0;

        if (!parse_result_record(line,
                                 &task_id,
                                 &worker_id,
                                 &exit_status,
                                 &state))
        {
            log_warn("wal_result_invalid",
                     "Skipping malformed RESULT record: %s",
                     line);
            return 1;
        }

        if (ctx->result_count >= RECOVERY_MAX_TASKS)
        {
            ctx->overflow = 1;
            return 1;
        }

        ctx->results[ctx->result_count].task_id = task_id;
        ctx->results[ctx->result_count].worker_id = worker_id;
        ctx->results[ctx->result_count].exit_status = exit_status;
        ctx->results[ctx->result_count].state = state;
        ctx->result_count++;

        if (task_id > ctx->max_task_id)
        {
            ctx->max_task_id = task_id;
        }
    }
    else
    {
        log_warn("wal_unknown_record",
                 "Skipping unknown WAL record: %s",
                 line);
    }

    return 1;
}

static struct RecoveredResult *find_result(
    struct RecoveryContext *ctx,
    int task_id)
{
    for (int i = 0; i < ctx->result_count; i++)
    {
        if (ctx->results[i].task_id == task_id)
        {
            return &ctx->results[i];
        }
    }

    return NULL;
}

int recovery_replay_wal(struct Runtime *runtime,
                        struct InFlightTable *table,
                        const char *path)
{
    if (runtime == NULL || table == NULL || path == NULL)
    {
        return -1;
    }

    static struct RecoveryContext ctx;
    memset(&ctx, 0, sizeof(ctx));

    int lines = wal_foreach_line(path, recovery_line_cb, &ctx);

    if (lines < 0)
    {
        log_error("wal_read_failed",
                  "Could not read WAL at %s",
                  path);
        return -1;
    }

    if (ctx.overflow)
    {
        log_warn("wal_recovery_truncated",
                 "WAL exceeded %d records; older entries not replayed",
                 RECOVERY_MAX_TASKS);
    }

    int requeued = 0;
    int completed = 0;

    for (int i = 0; i < ctx.submit_count; i++)
    {
        int task_id = ctx.submits[i].task_id;
        struct Task *task = &ctx.submits[i].task;

        runtime->stats.tasks_submitted++;

        struct RecoveredResult *res = find_result(&ctx, task_id);

        if (res != NULL)
        {
            coordinator_record_completed(table,
                                         task_id,
                                         res->worker_id,
                                         res->exit_status,
                                         res->state);

            if (res->state == (int)TASK_COMPLETED)
            {
                runtime->stats.tasks_completed++;
            }
            else
            {
                runtime->stats.tasks_failed++;
            }

            completed++;
        }
        else
        {
            if (submit_task(runtime->task_manager, task))
            {
                requeued++;
            }
        }
    }

    table->next_task_id = ctx.max_task_id + 1;

    log_info("wal_recovered",
             "Recovered %d queued and %d completed tasks from %s",
             requeued,
             completed,
             path);

    return requeued + completed;
}
