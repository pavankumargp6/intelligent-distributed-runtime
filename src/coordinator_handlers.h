#ifndef COORDINATOR_HANDLERS_H
#define COORDINATOR_HANDLERS_H

/* Forward declarations to avoid circular includes */
struct Runtime;
struct InFlightTable;
struct Connection;

int coordinator_handle_submit(struct Runtime *runtime,
                              struct InFlightTable *table,
                              struct Connection *conn,
                              const char *message);

int coordinator_handle_task_request(struct Runtime *runtime,
                                    struct InFlightTable *table,
                                    struct Connection *conn,
                                    const char *message);

int coordinator_handle_task_accept(struct Runtime *runtime,
                                   struct InFlightTable *table,
                                   struct Connection *conn,
                                   const char *message);

int coordinator_handle_task_result(struct Runtime *runtime,
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

int coordinator_reap_unaccepted_tasks(struct Runtime *runtime,
                                      struct InFlightTable *table);

/* Milestone H.2 */
int coordinator_reap_worker_tasks(struct Runtime *runtime,
                                  struct InFlightTable *table,
                                  int worker_id);

#endif
