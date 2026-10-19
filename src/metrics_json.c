#include <stdio.h>
#include "metrics_json.h"
#include "runtime.h"
#include "coordinator_tasks.h"
#include "runtime_stats.h"

int build_metrics_http_response(struct Runtime *runtime,
                                struct InFlightTable *table,
                                char *buffer,
                                size_t max_len)
{
    if (runtime == NULL || table == NULL || buffer == NULL)
    {
        return 0;
    }

    int queue = 0;
    if (runtime->task_manager != NULL)
    {
        queue = runtime->task_manager->queue.count;
    }

    int inflight = 0;
    for (int i = 0; i < COORDINATOR_MAX_INFLIGHT; i++)
    {
        if (table->entries[i].in_use)
        {
            inflight++;
        }
    }

    double avg_qw_ms = get_average_queue_wait(&runtime->stats) * 1000.0;
    double avg_exec_ms = get_average_execution_time(&runtime->stats) * 1000.0;

    /* 
     * We include Access-Control-Allow-Origin so a local HTML file
     * can fetch this data without CORS blocking it.
     */
    int n = snprintf(buffer, max_len,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\n"
        "  \"queue\": %d,\n"
        "  \"inflight\": %d,\n"
        "  \"submitted\": %d,\n"
        "  \"completed\": %d,\n"
        "  \"failed\": %d,\n"
        "  \"timed_out\": %d,\n"
        "  \"avg_queue_wait_ms\": %.2f,\n"
        "  \"avg_exec_ms\": %.2f\n"
        "}\n",
        queue, inflight,
        runtime->stats.tasks_submitted,
        runtime->stats.tasks_completed,
        runtime->stats.tasks_failed,
        runtime->stats.tasks_timed_out,
        avg_qw_ms, avg_exec_ms);

    if (n <= 0 || (size_t)n >= max_len)
    {
        return 0;
    }

    return 1;
}
