#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "recovery.h"
#include "wal.h"
#include "wire_fields.h"
#include "log.h"

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

    if (*cursor == '|')
    {
        cursor++;
        long idem = 0;
        if (wire_read_int(&cursor, &idem))
        {
            task.idempotent = idem ? 1 : 0;
        }
    }

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

    /*
     * Refuse to start if the WAL exceeds our recovery capacity.
     *
     * The previous behavior was to log a warning and continue
     * with a partially-recovered state, silently dropping older
     * tasks. That is not acceptable for a durability feature:
     * losing tasks without telling anyone is worse than failing
     * loudly.
     */
    if (ctx.overflow)
    {
        log_error("wal_recovery_overflow",
                  "WAL at %s has more than %d records; refusing to "
                  "recover. Compact the WAL or reduce the workload "
                  "before restarting.",
                  path,
                  RECOVERY_MAX_TASKS);
        return -1;
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

struct CompactEntry
{
    int task_id;
    int has_submit;
    int has_result;
};

struct CompactIndex
{
    struct CompactEntry entries[RECOVERY_MAX_TASKS];
    int count;

    /*
     * Set when a unique task_id arrives that does not fit in
     * entries[]. If this happens, compaction refuses to run —
     * silently dropping the extra records would leave the
     * coordinator running with less state than the WAL
     * represents.
     */
    int overflow;
};

static int compact_index_find(struct CompactIndex *idx,
                              int task_id)
{
    for (int i = 0; i < idx->count; i++)
    {
        if (idx->entries[i].task_id == task_id)
        {
            return i;
        }
    }

    return -1;
}

static int compact_index_upsert(struct CompactIndex *idx,
                                int task_id,
                                int has_submit,
                                int has_result)
{
    int pos = compact_index_find(idx, task_id);

    if (pos >= 0)
    {
        if (has_submit)
        {
            idx->entries[pos].has_submit = 1;
        }
        if (has_result)
        {
            idx->entries[pos].has_result = 1;
        }
        return 1;
    }

    if (idx->count >= RECOVERY_MAX_TASKS)
    {
        idx->overflow = 1;
        return 0;
    }

    idx->entries[idx->count].task_id = task_id;
    idx->entries[idx->count].has_submit = has_submit;
    idx->entries[idx->count].has_result = has_result;
    idx->count++;

    return 1;
}

static int compact_scan_cb(const char *line, void *user)
{
    struct CompactIndex *idx = user;
    int task_id = 0;

    if (sscanf(line, "SUBMIT|%d", &task_id) == 1)
    {
        compact_index_upsert(idx, task_id, 1, 0);
    }
    else if (sscanf(line, "RESULT|%d", &task_id) == 1)
    {
        compact_index_upsert(idx, task_id, 0, 1);
    }

    return 1;
}

struct CompactWriteCtx
{
    struct CompactIndex *idx;
    FILE *out;
    int lines_written;
};

static int compact_write_cb(const char *line, void *user)
{
    struct CompactWriteCtx *wctx = user;
    int task_id = 0;
    int is_result = 0;

    if (sscanf(line, "SUBMIT|%d", &task_id) == 1)
    {
        is_result = 0;
    }
    else if (sscanf(line, "RESULT|%d", &task_id) == 1)
    {
        is_result = 1;
    }
    else
    {
        return 1;
    }

    int pos = compact_index_find(wctx->idx, task_id);

    if (pos < 0)
    {
        return 1;
    }

    int keep = 0;

    if (is_result)
    {
        keep = 1;
    }
    else if (!wctx->idx->entries[pos].has_result)
    {
        keep = 1;
    }

    if (keep)
    {
        fprintf(wctx->out, "%s\n", line);
        wctx->lines_written++;
    }

    return 1;
}

int recovery_compact_wal(const char *path)
{
    if (path == NULL)
    {
        return 0;
    }

    if (access(path, F_OK) != 0)
    {
        return 1;
    }

    static struct CompactIndex idx;
    memset(&idx, 0, sizeof(idx));

    if (wal_foreach_line(path, compact_scan_cb, &idx) < 0)
    {
        log_error("wal_compact_scan_failed",
                  "Could not scan WAL at %s", path);
        return 0;
    }

    if (idx.overflow)
    {
        log_error("wal_compact_index_full",
                  "WAL at %s has more than %d unique tasks; "
                  "cannot compact safely",
                  path,
                  RECOVERY_MAX_TASKS);
        return 0;
    }

    char tmp_path[512];
    int n = snprintf(tmp_path,
                     sizeof(tmp_path),
                     "%s.compact",
                     path);

    if (n <= 0 || (size_t)n >= sizeof(tmp_path))
    {
        log_error("wal_compact_path_too_long",
                  "Cannot build temp path for %s", path);
        return 0;
    }

    FILE *out = fopen(tmp_path, "w");

    if (out == NULL)
    {
        log_error("wal_compact_open_failed",
                  "Could not open %s for writing", tmp_path);
        return 0;
    }

    struct CompactWriteCtx wctx = {
        .idx = &idx,
        .out = out,
        .lines_written = 0
    };

    if (wal_foreach_line(path, compact_write_cb, &wctx) < 0)
    {
        fclose(out);
        unlink(tmp_path);
        return 0;
    }

    if (fflush(out) != 0)
    {
        fclose(out);
        unlink(tmp_path);
        return 0;
    }

    if (fsync(fileno(out)) != 0)
    {
        fclose(out);
        unlink(tmp_path);
        return 0;
    }

    if (fclose(out) != 0)
    {
        unlink(tmp_path);
        return 0;
    }

    if (rename(tmp_path, path) != 0)
    {
        log_error("wal_compact_rename_failed",
                  "Could not rename %s to %s",
                  tmp_path,
                  path);
        unlink(tmp_path);
        return 0;
    }

    log_info("wal_compacted",
             "Compacted WAL at %s: %d unique tasks, %d lines",
             path,
             idx.count,
             wctx.lines_written);

    return 1;
}
