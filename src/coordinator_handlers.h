#ifndef COORDINATOR_HANDLERS_H
#define COORDINATOR_HANDLERS_H

#include "runtime.h"
#include "coordinator_tasks.h"

int coordinator_handle_submit(struct Runtime *runtime,
                              struct InFlightTable *table,
                              int client_socket,
                              const char *message);

int coordinator_handle_task_request(struct Runtime *runtime,
                                    struct InFlightTable *table,
                                    int worker_socket,
                                    const char *message);

int coordinator_handle_task_result(struct Runtime *runtime,
                                   struct InFlightTable *table,
                                   int worker_socket,
                                   const char *message);

int coordinator_handle_task_accept(struct Runtime *runtime,
                                   struct InFlightTable *table,
                                   int worker_socket,
                                   const char *message);

int coordinator_handle_query(struct Runtime *runtime,
                             struct InFlightTable *table,
                             int client_socket,
                             const char *message);

int coordinator_handle_status(struct Runtime *runtime,
                              struct InFlightTable *table,
                              int client_socket);

int coordinator_reap_dead_worker_tasks(struct Runtime *runtime,
                                       struct InFlightTable *table);

/*
 * Reap in-flight tasks that were dispatched but never
 * accepted within DISPATCH_TIMEOUT_SECONDS.
 *
 * Idempotent tasks are re-queued (they will be re-dispatched
 * when a worker next polls). Non-idempotent tasks are marked
 * TASK_FAILED — we will not risk duplicate side effects.
 *
 * Returns the number of tasks processed (requeued + failed).
 */
int coordinator_reap_unaccepted_tasks(struct Runtime *runtime,
                                      struct InFlightTable *table);

#endif
