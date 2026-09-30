#include <stddef.h>

#include "task_queue.h"

void initialize_task_queue(struct TaskQueue *queue)
{
    if (queue == NULL)
    {
        return;
    }

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
        (queue->rear + 1) % TASK_QUEUE_CAPACITY;

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
        (queue->front + 1) % TASK_QUEUE_CAPACITY;

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

    return queue->count == TASK_QUEUE_CAPACITY;
}
