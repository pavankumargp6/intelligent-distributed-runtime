#ifndef METRICS_JSON_H
#define METRICS_JSON_H

#include <stddef.h>

struct Runtime;
struct InFlightTable;

/*
 * Builds a complete HTTP 200 OK response containing a JSON
 * representation of the runtime metrics.
 *
 * Returns 1 on success, 0 on failure.
 */
int build_metrics_http_response(struct Runtime *runtime,
                                struct InFlightTable *table,
                                char *buffer,
                                size_t max_len);

#endif
