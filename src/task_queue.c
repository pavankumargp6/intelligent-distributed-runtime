#include <stddef.h>
#include <stdlib.h>

#include "task_queue.h"

int initialize_task_queue(
    struct TaskQueue *queue,
    size_t capacity)
{
    if (queue == NULL || capacity == 0)
    {
        return 0;
    }

    queue->tasks = calloc(capacity, sizeof(struct Task));
    if (queue->tasks == NULL)
    {
        queue->capacity = 0;
        queue->front = 0;
        queue->rear = 0;
        queue->count = 0;
        return 0;
    }

    queue->capacity = capacity;
    queue->front = 0;
    queue->rear = 0;
    queue->count = 0;

    return 1;
}

void destroy_task_queue(
    struct TaskQueue *queue)
{
    if (queue == NULL)
    {
        return;
    }

    free(queue->tasks);
    queue->tasks = NULL;
    queue->capacity = 0;
    queue->front = 0;
    queue->rear = 0;
    queue->count = 0;
}

int enqueue_task(struct TaskQueue *queue,
                 struct Task *task)
{
    if (queue == NULL || task == NULL)
    {
        return 0;
    }

    if (is_task_queue_full(queue))
    {
        return 0;
    }

    queue->tasks[queue->rear] = *task;

    queue->rear =
        (queue->rear + 1) % (int)queue->capacity;

    queue->count++;

    return 1;
}

int dequeue_task(struct TaskQueue *queue,
                 struct Task *task)
{
    if (queue == NULL || task == NULL)
    {
        return 0;
    }

    if (is_task_queue_empty(queue))
    {
        return 0;
    }

    *task = queue->tasks[queue->front];

    queue->front =
        (queue->front + 1) % (int)queue->capacity;

    queue->count--;

    return 1;
}

int is_task_queue_empty(struct TaskQueue *queue)
{
    if (queue == NULL)
    {
        return 1;
    }

    return queue->count == 0;
}

int is_task_queue_full(struct TaskQueue *queue)
{
    if (queue == NULL)
    {
        return 0;
    }

    return queue->count == (int)queue->capacity;
}

int task_queue_peek(const struct TaskQueue *queue,
                    int index,
                    struct Task *task)
{
    if (queue == NULL || task == NULL ||
        index < 0 || index >= queue->count)
    {
        return 0;
    }

    int position = (queue->front + index) % (int)queue->capacity;

    *task = queue->tasks[position];

    return 1;
}

int task_queue_contains(const struct TaskQueue *queue,
                        int task_id)
{
    if (queue == NULL)
    {
        return 0;
    }

    for (int i = 0; i < queue->count; i++)
    {
        int position = (queue->front + i) % (int)queue->capacity;

        if (queue->tasks[position].id == task_id)
        {
            return 1;
        }
    }

    return 0;
}