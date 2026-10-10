#ifndef COORDINATOR_HANDLERS_H
#define COORDINATOR_HANDLERS_H

#include "runtime.h"
#include "coordinator_tasks.h"

/*
 * Handle a client SUBMIT message:
 *
 *   SUBMIT|<cmd_len>:<cmd>|<argc>|<arg1>|...
 *
 * Replies SUBMIT_ACK|<task_id> or SUBMIT_REJECT.
 */
int coordinator_handle_submit(struct Runtime *runtime,
                              struct InFlightTable *table,
                              int client_socket,
                              const char *message);

/*
 * Handle a worker TASK_REQUEST message:
 *
 *   TASK_REQUEST|<worker_id>
 *
 * Replies TASK_ASSIGN|... or NO_TASK.
 */
int coordinator_handle_task_request(struct Runtime *runtime,
                                    struct InFlightTable *table,
                                    int worker_socket,
                                    const char *message);

/*
 * Handle a worker TASK_RESULT message:
 *
 *   TASK_RESULT|<task_id>|<worker_id>|<exit_status>|<state>
 *
 * Replies TASK_RESULT_ACK.
 */
int coordinator_handle_task_result(struct Runtime *runtime,
                                   struct InFlightTable *table,
                                   int worker_socket,
                                   const char *message);

/*
 * Handle a client QUERY message:
 *
 *   QUERY|<task_id>
 *
 * Replies one of:
 *
 *   QUERY_RESULT|<task_id>|<worker_id>|<exit_status>|<state>
 *   QUERY_PENDING|<task_id>
 *   QUERY_UNKNOWN|<task_id>
 */
int coordinator_handle_query(struct Runtime *runtime,
                             struct InFlightTable *table,
                             int client_socket,
                             const char *message);


/*
 * Reap in-flight tasks whose assigned worker is no longer live.
 *
 * Called from the coordinator poll tick, after
 * mark_stale_workers, so a worker going stale causes its
 * dispatched-but-unreported tasks to be recorded as
 * TASK_FAILED. No retries: a task with side effects must
 * not be silently re-executed.
 *
 * Returns the number of tasks reaped.
 */
int coordinator_reap_dead_worker_tasks(struct Runtime *runtime,
                                       struct InFlightTable *table);
#endif
