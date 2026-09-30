#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include "task.h"

#define TASK_QUEUE_CAPACITY 64

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

#endif
