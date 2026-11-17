#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "coordinator_tasks.h"

/*
 * Default terminal-result retention.
 *
 * This is deliberately a count and an age bound: a busy
 * deployment with a million short tasks should not pin a
 * gigabyte of history just because none of it is seven days old
 * yet. Both values are overridable by environment variables so
 * the test suite can use a small table.
 */
#define DEFAULT_RESULT_RETENTION_COUNT 100000u
#define DEFAULT_RESULT_RETENTION_SECONDS (7 * 24 * 60 * 60)

static size_t env_size(const char *name, size_t fallback,
                       size_t minimum, size_t maximum)
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

    return (size_t)parsed;
}

static size_t next_power_of_two(size_t value)
{
    size_t result = 1;

    while (result < value)
    {
        if (result > SIZE_MAX / 2)
        {
            return 0;
        }

        result <<= 1;
    }

    return result;
}

static size_t history_hash(int task_id, size_t mask)
{
    uint32_t x = (uint32_t)task_id;

    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;

    return (size_t)x & mask;
}

static void history_index_put(struct TaskHistory *history,
                              int task_id,
                              size_t position)
{
    if (history->slot_count == 0 || history->slots == NULL)
    {
        return;
    }

    size_t mask = history->slot_count - 1;
    size_t index = history_hash(task_id, mask);
    size_t first_tombstone = SIZE_MAX;

    for (size_t probe = 0; probe <= mask; probe++)
    {
        int slot = history->slots[index];

        if (slot == 0)
        {
            size_t target = first_tombstone != SIZE_MAX
                                ? first_tombstone
                                : index;

            history->slots[target] = (int)position + 1;
            return;
        }

        if (slot == -1 && first_tombstone == SIZE_MAX)
        {
            first_tombstone = index;
        }
        else if (slot > 0 && (size_t)(slot - 1) == position)
        {
            return;
        }

        index = (index + 1) & mask;
    }

    /*
     * The probe found no empty slot. This should not happen
     * because slots >= 2 * capacity, but reusing a tombstone is
     * still correct when one exists.
     */
    if (first_tombstone != SIZE_MAX)
    {
        history->slots[first_tombstone] = (int)position + 1;
    }
}

static void history_index_remove(struct TaskHistory *history,
                                 int task_id,
                                 size_t position)
{
    if (history->slot_count == 0 || history->slots == NULL)
    {
        return;
    }

    size_t mask = history->slot_count - 1;
    size_t index = history_hash(task_id, mask);

    for (size_t probe = 0; probe <= mask; probe++)
    {
        int slot = history->slots[index];

        if (slot == 0)
        {
            return;
        }

        if (slot > 0 && (size_t)(slot - 1) == position)
        {
            history->slots[index] = -1;
            history->tombstones++;
            return;
        }

        index = (index + 1) & mask;
    }
}

static void history_rebuild_index(struct TaskHistory *history)
{
    if (history->slots == NULL || history->slot_count == 0)
    {
        return;
    }

    memset(history->slots, 0,
           history->slot_count * sizeof(history->slots[0]));

    history->tombstones = 0;

    for (size_t i = 0; i < history->count; i++)
    {
        size_t position = (history->head + i) % history->capacity;
        history_index_put(history,
                          history->entries[position].task_id,
                          position);
    }
}

static void history_remove_oldest(struct TaskHistory *history)
{
    if (history->count == 0)
    {
        return;
    }

    size_t position = history->head;

    history_index_remove(history,
                         history->entries[position].task_id,
                         position);

    history->head = (history->head + 1) % history->capacity;
    history->count--;
}

int coordinator_history_init(struct TaskHistory *history,
                             size_t capacity,
                             time_t retention_seconds)
{
    if (history == NULL)
    {
        return 0;
    }

    memset(history, 0, sizeof(*history));

    history->retention_seconds = retention_seconds;

    if (capacity < COORDINATOR_MAX_COMPLETED)
    {
        capacity = COORDINATOR_MAX_COMPLETED;
    }

    size_t slots = next_power_of_two(capacity * 2);

    if (slots == 0)
    {
        return 0;
    }

    history->entries = calloc(capacity, sizeof(history->entries[0]));
    history->slots = calloc(slots, sizeof(history->slots[0]));

    if (history->entries == NULL || history->slots == NULL)
    {
        free(history->entries);
        free(history->slots);
        memset(history, 0, sizeof(*history));
        return 0;
    }

    history->capacity = capacity;
    history->slot_count = slots;

    return 1;
}

void coordinator_history_destroy(struct TaskHistory *history)
{
    if (history == NULL)
    {
        return;
    }

    free(history->entries);
    free(history->slots);
    memset(history, 0, sizeof(*history));
}

int coordinator_history_lookup(const struct TaskHistory *history,
                               int task_id,
                               struct HistoryRecord *out)
{
    if (history == NULL || history->slots == NULL ||
        history->slot_count == 0 || history->count == 0)
    {
        return 0;
    }

    size_t mask = history->slot_count - 1;
    size_t index = history_hash(task_id, mask);

    for (size_t probe = 0; probe <= mask; probe++)
    {
        int slot = history->slots[index];

        if (slot == 0)
        {
            return 0;
        }

        if (slot > 0)
        {
            size_t position = (size_t)(slot - 1);

            if (position < history->capacity &&
                history->entries[position].task_id == task_id)
            {
                if (out != NULL)
                {
                    *out = history->entries[position];
                }

                return 1;
            }
        }

        index = (index + 1) & mask;
    }

    return 0;
}

int coordinator_history_record(struct TaskHistory *history,
                               int task_id,
                               int worker_id,
                               int exit_status,
                               int state,
                               time_t completed_at)
{
    if (history == NULL || history->capacity == 0 ||
        history->entries == NULL || history->slots == NULL)
    {
        return 0;
    }

    if (coordinator_history_lookup(history, task_id, NULL))
    {
        return 0;
    }

    if (history->retention_seconds > 0)
    {
        while (history->count > 0)
        {
            time_t oldest = history->entries[history->head].completed_at;

            if (difftime(completed_at, oldest) <=
                (double)history->retention_seconds)
            {
                break;
            }

            history_remove_oldest(history);
        }
    }

    if (history->count >= history->capacity)
    {
        history_remove_oldest(history);
    }

    if (history->tombstones > history->capacity / 2)
    {
        history_rebuild_index(history);
    }

    size_t position = (history->head + history->count) %
                      history->capacity;

    history->entries[position].task_id = task_id;
    history->entries[position].worker_id = worker_id;
    history->entries[position].exit_status = exit_status;
    history->entries[position].state = state;
    history->entries[position].completed_at = completed_at;

    history_index_put(history, task_id, position);
    history->count++;

    return 1;
}

size_t coordinator_history_count(const struct TaskHistory *history)
{
    if (history == NULL)
    {
        return 0;
    }

    return history->count;
}

int coordinator_history_get(const struct TaskHistory *history,
                            size_t index,
                            struct HistoryRecord *out)
{
    if (history == NULL || out == NULL ||
        history->capacity == 0 || index >= history->count)
    {
        return 0;
    }

    size_t position = (history->head + index) % history->capacity;

    *out = history->entries[position];

    return 1;
}

void coordinator_tasks_init(struct InFlightTable *table)
{
    if (table == NULL)
    {
        return;
    }

    memset(table, 0, sizeof(*table));
    table->next_task_id = 1;
    table->completed_next = 0;

    size_t capacity = env_size("IDR_RESULT_RETENTION",
                               DEFAULT_RESULT_RETENTION_COUNT,
                               COORDINATOR_MAX_COMPLETED,
                               10000000u);

    size_t ttl = env_size("IDR_RESULT_RETENTION_SECONDS",
                          DEFAULT_RESULT_RETENTION_SECONDS,
                          0,
                          10u * 365u * 24u * 60u * 60u);

    (void)coordinator_history_init(&table->history,
                                   capacity,
                                   (time_t)ttl);
}

void coordinator_tasks_destroy(struct InFlightTable *table)
{
    if (table == NULL)
    {
        return;
    }

    coordinator_history_destroy(&table->history);
}

int coordinator_allocate_task_id(struct InFlightTable *table)
{
    if (table == NULL)
    {
        return -1;
    }

    int id = table->next_task_id;
    table->next_task_id++;

    return id;
}

int coordinator_record_inflight(struct InFlightTable *table,
                                const struct Task *task,
                                int worker_id)
{
    if (table == NULL || task == NULL)
    {
        return 0;
    }

    int i;
    for (i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (!table->entries[i].in_use)
        {
            table->entries[i].in_use = 1;
            table->entries[i].task_id = task->id;
            table->entries[i].worker_id = worker_id;
            table->entries[i].task = *task;
            table->entries[i].dispatched_at = time(NULL);
            table->entries[i].accepted = 0;
            return 1;
        }
    }

    return 0;
}

int coordinator_take_inflight(struct InFlightTable *table,
                              int task_id,
                              struct Task *task,
                              int *worker_id)
{
    if (table == NULL)
    {
        return 0;
    }

    int i;
    for (i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (table->entries[i].in_use &&
            table->entries[i].task_id == task_id)
        {
            if (task != NULL)
            {
                *task = table->entries[i].task;
            }

            if (worker_id != NULL)
            {
                *worker_id = table->entries[i].worker_id;
            }

            table->entries[i].in_use = 0;
            return 1;
        }
    }

    return 0;
}

int coordinator_peek_inflight(struct InFlightTable *table,
                              int task_id,
                              struct Task *task,
                              int *worker_id)
{
    if (table == NULL)
    {
        return 0;
    }

    int i;
    for (i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (table->entries[i].in_use &&
            table->entries[i].task_id == task_id)
        {
            if (task != NULL)
            {
                *task = table->entries[i].task;
            }

            if (worker_id != NULL)
            {
                *worker_id = table->entries[i].worker_id;
            }

            return 1;
        }
    }

    return 0;
}

int coordinator_is_inflight(struct InFlightTable *table,
                            int task_id)
{
    if (table == NULL)
    {
        return 0;
    }

    int i;
    for (i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (table->entries[i].in_use &&
            table->entries[i].task_id == task_id)
        {
            return 1;
        }
    }

    return 0;
}

int coordinator_mark_accepted(struct InFlightTable *table,
                              int task_id)
{
    return coordinator_mark_accepted_worker(table, task_id, NULL, NULL);
}

int coordinator_mark_accepted_worker(struct InFlightTable *table,
                                     int task_id,
                                     int *worker_id,
                                     int *attempt)
{
    if (table == NULL)
    {
        return 0;
    }

    int i;
    for (i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (table->entries[i].in_use &&
            table->entries[i].task_id == task_id)
        {
            table->entries[i].accepted = 1;

            if (worker_id != NULL)
            {
                *worker_id = table->entries[i].worker_id;
            }

            if (attempt != NULL)
            {
                *attempt = table->entries[i].task.attempt;
            }

            return 1;
        }
    }

    return 0;
}

void coordinator_record_completed_at(struct InFlightTable *table,
                                     int task_id,
                                     int worker_id,
                                     int exit_status,
                                     int state,
                                     time_t completed_at)
{
    if (table == NULL)
    {
        return;
    }

    struct CompletedRecord *slot =
        &table->completed[table->completed_next];

    slot->in_use = 1;
    slot->task_id = task_id;
    slot->worker_id = worker_id;
    slot->exit_status = exit_status;
    slot->state = state;

    table->completed_next =
        (table->completed_next + 1) % COORDINATOR_MAX_COMPLETED;

    (void)coordinator_history_record(&table->history,
                                     task_id,
                                     worker_id,
                                     exit_status,
                                     state,
                                     completed_at);
}

void coordinator_record_completed(struct InFlightTable *table,
                                  int task_id,
                                  int worker_id,
                                  int exit_status,
                                  int state)
{
    coordinator_record_completed_at(table,
                                    task_id,
                                    worker_id,
                                    exit_status,
                                    state,
                                    time(NULL));
}

int coordinator_lookup_completed(struct InFlightTable *table,
                                 int task_id,
                                 int *worker_id,
                                 int *exit_status,
                                 int *state)
{
    if (table == NULL)
    {
        return 0;
    }

    struct HistoryRecord record;

    if (coordinator_history_lookup(&table->history, task_id, &record))
    {
        if (worker_id != NULL)
        {
            *worker_id = record.worker_id;
        }
        if (exit_status != NULL)
        {
            *exit_status = record.exit_status;
        }
        if (state != NULL)
        {
            *state = record.state;
        }
        return 1;
    }

    int i;
    for (i = 0; i < COORDINATOR_MAX_COMPLETED; i++)
    {
        if (table->completed[i].in_use &&
            table->completed[i].task_id == task_id)
        {
            if (worker_id != NULL)
            {
                *worker_id = table->completed[i].worker_id;
            }
            if (exit_status != NULL)
            {
                *exit_status = table->completed[i].exit_status;
            }
            if (state != NULL)
            {
                *state = table->completed[i].state;
            }
            return 1;
        }
    }

    return 0;
}
