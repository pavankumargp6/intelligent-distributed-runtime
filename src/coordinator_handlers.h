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

#endif
