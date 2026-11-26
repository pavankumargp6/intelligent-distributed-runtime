#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include "task.h"

struct TaskQueue
{
    struct Task *tasks;
    size_t capacity;
    int front;
    int rear;
    int count;
};

/*
 * Initialize the queue with the given capacity.
 *
 * Capacity must be > 0. On allocation failure the queue
 * is left in a zeroed state and the function returns 0.
 */
int initialize_task_queue(
    struct TaskQueue *queue,
    size_t capacity);

/*
 * Destroy the queue and free allocated memory.
 */
void destroy_task_queue(
    struct TaskQueue *queue);

int enqueue_task(struct TaskQueue *queue,
                 struct Task *task);

int dequeue_task(struct TaskQueue *queue,
                 struct Task *task);

int is_task_queue_empty(struct TaskQueue *queue);

int is_task_queue_full(struct TaskQueue *queue);

/*
 * Copy the task at logical position index (0 = front) without
 * removing it.
 */
int task_queue_peek(const struct TaskQueue *queue,
                    int index,
                    struct Task *task);

/*
 * Return 1 when a task with the given ID is currently queued.
 */
int task_queue_contains(const struct TaskQueue *queue,
                        int task_id);

#endif