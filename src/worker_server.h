#ifndef WORKER_SERVER_H
#define WORKER_SERVER_H

#include "worker.h"

/*
 * Start the TCP server for a worker.
 *
 * The server listens on the given port
 * and waits for an incoming connection.
 */
int start_worker_server(struct Worker *worker,
                        int port);

#endif
