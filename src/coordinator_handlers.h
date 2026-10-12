#ifndef COORDINATOR_HANDLERS_H
#define COORDINATOR_HANDLERS_H

#include "runtime.h"
#include "coordinator_tasks.h"
#include "connection_pool.h"

int coordinator_handle_submit(struct Runtime *runtime,
                              struct InFlightTable *table,
                              struct Connection *conn,
                              const char *message);

int coordinator_handle_task_request(struct Runtime *runtime,
                                    struct InFlightTable *table,
                                    struct Connection *conn,
                                    const char *message);

int coordinator_handle_task_result(struct Runtime *runtime,
                                   struct InFlightTable *table,
                                   struct Connection *conn,
                                   const char *message);

int coordinator_handle_task_accept(struct Runtime *runtime,
                                   struct InFlightTable *table,
                                   struct Connection *conn,
                                   const char *message);

int coordinator_handle_query(struct Runtime *runtime,
                             struct InFlightTable *table,
                             struct Connection *conn,
                             const char *message);

int coordinator_handle_status(struct Runtime *runtime,
                              struct InFlightTable *table,
                              struct Connection *conn);

int coordinator_reap_dead_worker_tasks(struct Runtime *runtime,
                                       struct InFlightTable *table);

/*
 * Reap in-flight tasks that were dispatched but never
 * accepted within DISPATCH_TIMEOUT_SECONDS.
 */
int coordinator_reap_unaccepted_tasks(struct Runtime *runtime,
                                      struct InFlightTable *table);

#endif
