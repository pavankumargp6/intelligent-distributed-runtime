#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include "task.h"

#define TASK_QUEUE_CAPACITY 512

struct TaskQueue
{
    struct Task tasks[TASK_QUEUE_CAPACITY];

    int front;
    int rear;
    int count;
};

void initialize_task_queue(struct TaskQueue *queue);

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
