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

int coordinator_handle_query(struct Runtime *runtime,
                             struct InFlightTable *table,
                             int client_socket,
                             const char *message);

/*
 * Handle a STATUS message.
 *
 * Replies:
 *
 *   STATUS_REPLY|workers=N|active=N|queue=N|inflight=N|
 *                submitted=N|completed=N|failed=N
 */
int coordinator_handle_status(struct Runtime *runtime,
                              struct InFlightTable *table,
                              int client_socket);

int coordinator_reap_dead_worker_tasks(struct Runtime *runtime,
                                       struct InFlightTable *table);

#endif
