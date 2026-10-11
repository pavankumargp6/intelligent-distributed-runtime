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

/*
 * Handle a TASK_ACCEPT message from a worker.
 *
 *   TASK_ACCEPT|<task_id>
 *
 * Marks the in-flight entry as accepted so the dispatch
 * timeout reaper leaves it alone. Replies TASK_ACCEPT_ACK
 * or TASK_ACCEPT_REJECT.
 */
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

#endif
