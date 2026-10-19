#!/bin/bash
#
# IDR runtime benchmark.
#
# Parameterized harness. Runs a coordinator, N workers, and M
# client processes in parallel. Each client submits tasks/N
# submissions. When all clients finish, the script waits for
# the coordinator to observe every task's completion, then
# parses the coordinator log to compute latency percentiles
# and writes one CSV row.
#
# Usage:
#   bench_runtime.sh [--clients=N] [--workers=N] [--tasks=N]
#                    [--workload=true|sleep1] [--output=PATH]
#
# Defaults: clients=1 workers=1 tasks=1000 workload=true
#           output=/tmp/idr_bench_results.csv
#
# Metrics:
#   end-to-end latency   coordinator accepts SUBMIT -> processes RESULT
#                        (from task_latency event, end_to_end_ms)
#   queue wait           coordinator queues task -> dispatches to worker
#                        (from task_latency event, queue_wait_ms)
#   submission rate      tasks submitted per second (aggregate client window)
#   completion rate      tasks completed per second (coordinator wall time)
#   connection count     number of accept() events in coordinator log
#
# Fails if:
#   - any client exits non-zero
#   - the coordinator does not reach the expected completion count
#     within the timeout
#   - any task_latency event reports a state other than 2 (COMPLETED)

set -u

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

CLIENTS=1
WORKERS=1
TASKS=1000
WORKLOAD=true
AGENT_INTERVAL=0
AGENT_INTERVAL=0
OUTPUT_CSV=/tmp/idr_bench_results.csv

for arg in "$@"; do
    case "$arg" in
        --clients=*)  CLIENTS="${arg#*=}" ;;
        --workers=*)  WORKERS="${arg#*=}" ;;
        --tasks=*)    TASKS="${arg#*=}" ;;
        --workload=*) WORKLOAD="${arg#*=}" ;;
        --agent-interval=*) AGENT_INTERVAL="${arg#*=}" ;;
        --output=*)   OUTPUT_CSV="${arg#*=}" ;;
        --agent-interval=*) AGENT_INTERVAL="${arg#*=}" ;;
        -h|--help)
            sed -n '3,30p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

if ! [[ "$CLIENTS" =~ ^[0-9]+$ ]] || [ "$CLIENTS" -lt 1 ]; then
    echo "clients must be a positive integer" >&2
    exit 2
fi

if ! [[ "$WORKERS" =~ ^[0-9]+$ ]] || [ "$WORKERS" -lt 1 ]; then
    echo "workers must be a positive integer" >&2
    exit 2
fi

if ! [[ "$TASKS" =~ ^[0-9]+$ ]] || [ "$TASKS" -lt 1 ]; then
    echo "tasks must be a positive integer" >&2
    exit 2
fi

if [ "$WORKLOAD" != "true" ] && [ "$WORKLOAD" != "sleep1" ]; then
    echo "workload must be true or sleep1" >&2
    exit 2
fi

if [ $((TASKS % CLIENTS)) -ne 0 ]; then
    echo "tasks ($TASKS) must be divisible by clients ($CLIENTS)" >&2
    exit 2
fi

if [ "$WORKLOAD" = "sleep1" ] && [ "$TASKS" -gt 500 ]; then
    echo "WARNING: workload=sleep1 with tasks=$TASKS will spawn up"
    echo "         to $TASKS concurrent forked children. Consider a"
    echo "         smaller task count."
    echo
fi

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------

cd "$(dirname "$0")/.."

RUN_ID=$$
WAL_PATH="/tmp/idr_wal_bench_${RUN_ID}.log"
COORD_LOG="/tmp/idr_coord_bench_${RUN_ID}.log"
WORKER_LOG_PREFIX="/tmp/idr_worker_bench_${RUN_ID}"
CLIENT_OUT_PREFIX="/tmp/idr_client_bench_${RUN_ID}"
CLIENT_IDS_PREFIX="/tmp/idr_ids_bench_${RUN_ID}"

COORD_BIN="/tmp/idr_coord_bench"
WORKER_BIN="/tmp/idr_worker_bench"
CLIENT_BIN="/tmp/idr_bench_client"

COORD_PID=""
declare -a WORKER_PIDS=()
declare -a CLIENT_PIDS=()

cleanup() {
    for pid in "${CLIENT_PIDS[@]:-}"; do
        [ -n "$pid" ] && kill -KILL "$pid" 2>/dev/null
    done
    for pid in "${WORKER_PIDS[@]:-}"; do
        [ -n "$pid" ] && kill -KILL "$pid" 2>/dev/null
    done
    [ -n "$COORD_PID" ] && kill -KILL "$COORD_PID" 2>/dev/null

    for pid in "${CLIENT_PIDS[@]:-}"; do
        [ -n "$pid" ] && wait "$pid" 2>/dev/null
    done
    for pid in "${WORKER_PIDS[@]:-}"; do
        [ -n "$pid" ] && wait "$pid" 2>/dev/null
    done
    [ -n "$COORD_PID" ] && wait "$COORD_PID" 2>/dev/null

    rm -f "$WAL_PATH" "${WAL_PATH}.compact"
    rm -f "${WORKER_LOG_PREFIX}"_*.log
    rm -f "${CLIENT_OUT_PREFIX}"_*.out
    rm -f "${CLIENT_IDS_PREFIX}"_*.txt
    true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

echo "===== Building ====="

gcc -Wall -Wextra -Isrc \
    src/runtime_server_main.c src/runtime_server.c src/runtime.c \
    src/worker_registry.c src/task_manager.c src/task_queue.c \
    src/scheduler.c src/worker_pool.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c src/task_executor.c src/task_metrics.c \
    src/runtime_stats.c src/network_protocol.c src/worker_registration.c \
    src/coordinator_tasks.c src/coordinator_handlers.c src/wire_fields.c \
    src/log.c src/wal.c src/recovery.c src/metrics_json.c src/connection_pool.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$COORD_BIN" || exit 1

gcc -Wall -Wextra -Isrc \
    src/worker_agent.c src/worker.c src/worker_resources.c \
    src/process_info.c src/cpu_monitor.c \
    src/worker_registration.c src/worker_registration_client.c \
    src/worker_task_client.c src/wire_fields.c \
    src/task_executor.c src/task_metrics.c src/network_protocol.c \
    src/log.c src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$WORKER_BIN" || exit 1

gcc -Wall -Wextra -Isrc \
    tests/bench_client.c \
    src/network_protocol.c src/wire_fields.c \
    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
    -o "$CLIENT_BIN" || exit 1

# ---------------------------------------------------------------------------
# Start coordinator
# ---------------------------------------------------------------------------

# Use debug logging so connection_accepted events are recorded.
echo "===== Starting coordinator ====="

IDR_INSECURE=1 \
IDR_LOG_LEVEL=debug \
IDR_WAL_PATH="$WAL_PATH" \
    "$COORD_BIN" > "$COORD_LOG" 2>&1 &
COORD_PID=$!

sleep 1

if ! kill -0 "$COORD_PID" 2>/dev/null; then
    echo "FAIL: coordinator did not stay running." >&2
    cat "$COORD_LOG" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Start workers
# ---------------------------------------------------------------------------

echo "===== Starting $WORKERS worker(s) ====="

for ((i = 1; i <= WORKERS; i++)); do
    WORKER_LOG="${WORKER_LOG_PREFIX}_${i}.log"

    IDR_INSECURE=1 \
    IDR_AGENT_INTERVAL_SECONDS="$AGENT_INTERVAL" \
        "$WORKER_BIN" "$i" > "$WORKER_LOG" 2>&1 &
    WORKER_PIDS+=("$!")
done

# Wait for every worker to register (check coordinator log).
REGISTER_COUNT=0
for _ in $(seq 1 10); do
    REGISTER_COUNT=$(grep -c '"event":"worker_registered"' "$COORD_LOG" 2>/dev/null || :)
    if [ "$REGISTER_COUNT" -ge "$WORKERS" ]; then
        break
    fi
    sleep 1
done

if [ "$REGISTER_COUNT" -lt "$WORKERS" ]; then
    echo "FAIL: only $REGISTER_COUNT of $WORKERS workers registered" >&2
    cat "$COORD_LOG" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Run clients
# ---------------------------------------------------------------------------

TASKS_PER_CLIENT=$((TASKS / CLIENTS))

echo "===== Launching $CLIENTS client(s), $TASKS_PER_CLIENT tasks each ====="

# Record the wall clock just before we launch clients. We also use the
# client-reported nanosecond timestamps for the aggregate submission window.
WALL_START=$(date +%s)

for ((i = 1; i <= CLIENTS; i++)); do
    CLIENT_OUT="${CLIENT_OUT_PREFIX}_${i}.out"
    CLIENT_IDS="${CLIENT_IDS_PREFIX}_${i}.txt"

    IDR_INSECURE=1 \
        "$CLIENT_BIN" 127.0.0.1 "$TASKS_PER_CLIENT" "$WORKLOAD" "$CLIENT_IDS" \
        > "$CLIENT_OUT" 2>&1 &
    CLIENT_PIDS+=("$!")
done

# Wait for every client to finish.
CLIENT_FAILED=0
for pid in "${CLIENT_PIDS[@]}"; do
    if ! wait "$pid"; then
        CLIENT_FAILED=1
    fi
done

if [ "$CLIENT_FAILED" -ne 0 ]; then
    echo "FAIL: at least one client exited non-zero" >&2
    for f in "${CLIENT_OUT_PREFIX}"_*.out; do
        echo "--- $f ---"
        cat "$f"
    done
    exit 1
fi

WALL_END_SUBMIT=$(date +%s)
echo "All clients finished submitting."

# ---------------------------------------------------------------------------
# Wait for completions
# ---------------------------------------------------------------------------

echo "===== Waiting for tasks to complete (timeout 300s) ====="

COMPLETED=0
for _ in $(seq 1 300); do
    COMPLETED=$(grep -c '"event":"task_latency"' "$COORD_LOG" 2>/dev/null || :)
    if [ "$COMPLETED" -ge "$TASKS" ]; then
        break
    fi
    sleep 1
done

WALL_END_COMPLETE=$(date +%s)

if [ "$COMPLETED" -lt "$TASKS" ]; then
    echo "FAIL: $COMPLETED of $TASKS tasks completed before timeout" >&2
    echo "--- coordinator log (last 40 lines) ---"
    tail -40 "$COORD_LOG" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Verify no task failed
# ---------------------------------------------------------------------------

FAILED_TASKS=$(grep '"event":"task_latency"' "$COORD_LOG" | \
    grep -c 'state=3' || :)

if [ "$FAILED_TASKS" -ne 0 ]; then
    echo "FAIL: $FAILED_TASKS task(s) reported state=3 (FAILED)" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Compute metrics
# ---------------------------------------------------------------------------

# Extract queue_wait_ms and end_to_end_ms from task_latency events.
awk '
    /"event":"task_latency"/ {
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^queue_wait_ms=/) {
                split($i, a, "="); gsub(/[^0-9.]/, "", a[2])
                print a[2] > "/tmp/idr_bench_qw_'$RUN_ID'.txt"
            }
            if ($i ~ /^end_to_end_ms=/) {
                split($i, b, "="); gsub(/[^0-9.]/, "", b[2])
                print b[2] > "/tmp/idr_bench_e2e_'$RUN_ID'.txt"
            }
        }
    }
' "$COORD_LOG"

QW_FILE="/tmp/idr_bench_qw_${RUN_ID}.txt"
E2E_FILE="/tmp/idr_bench_e2e_${RUN_ID}.txt"

# Percentile helper. $1 = fraction (0.5, 0.95), $2 = file.
percentile() {
    local frac="$1"
    local file="$2"
    local n
    n=$(wc -l < "$file" 2>/dev/null || :)

    if [ "$n" -eq 0 ]; then
        echo "0"
        return
    fi

    local idx
    idx=$(awk -v f="$frac" -v n="$n" 'BEGIN { print int(f * (n - 1)) + 1 }')

    sort -n "$file" | sed -n "${idx}p"
}

QW_P50=$(percentile 0.5 "$QW_FILE")
QW_P95=$(percentile 0.95 "$QW_FILE")
E2E_P50=$(percentile 0.5 "$E2E_FILE")
E2E_P95=$(percentile 0.95 "$E2E_FILE")

rm -f "$QW_FILE" "$E2E_FILE"

# Aggregate submission throughput from client-reported nanosecond windows.
AGG_SUBMIT_THROUGHPUT=$(awk '
    BEGIN { min_start = -1; max_end = 0; total = 0 }
    /^submission_start_ns=/ {
        v = $0; sub(/^submission_start_ns=/, "", v)
        if (min_start < 0 || v + 0 < min_start) min_start = v + 0
    }
    /^submission_end_ns=/ {
        v = $0; sub(/^submission_end_ns=/, "", v)
        if (v + 0 > max_end) max_end = v + 0
    }
    /^tasks_submitted=/ {
        v = $0; sub(/^tasks_submitted=/, "", v)
        total += v + 0
    }
    END {
        if (total == 0 || max_end <= min_start) { print "0.00"; exit }
        elapsed_s = (max_end - min_start) / 1e9
        printf "%.2f", total / elapsed_s
    }
' "${CLIENT_OUT_PREFIX}"_*.out)

# Wall-clock completion throughput (submit-finish to all-complete).
WALL_SECONDS=$((WALL_END_COMPLETE - WALL_START))

COMPLETION_THROUGHPUT=$(awk -v t="$TASKS" -v s="$WALL_SECONDS" \
    'BEGIN { if (s <= 0) print "0.00"; else printf "%.2f", t / s }')

# Connection count.
CONNECTION_COUNT=$(grep -c '"event":"connection_accepted"' "$COORD_LOG" 2>/dev/null || :)

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

echo
echo "Scenario: clients=$CLIENTS workers=$WORKERS tasks=$TASKS command=$WORKLOAD"
echo "Completed: $COMPLETED/$TASKS"
echo "Submission throughput:  $AGG_SUBMIT_THROUGHPUT tasks/sec (client-reported window)"
echo "Completion throughput:  $COMPLETION_THROUGHPUT tasks/sec (wall-clock)"
echo "End-to-end latency:     p50=${E2E_P50}ms p95=${E2E_P95}ms"
echo "Queue wait:             p50=${QW_P50}ms p95=${QW_P95}ms"
echo "Connection count:       $CONNECTION_COUNT"
echo

# Write CSV header once.
if [ ! -f "$OUTPUT_CSV" ]; then
    echo "timestamp,scenario,workload,clients,workers,tasks,completed,submission_throughput,completion_throughput,e2e_p50_ms,e2e_p95_ms,queue_p50_ms,queue_p95_ms,connection_count,wall_seconds" > "$OUTPUT_CSV"
fi

TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)
SCENARIO="c${CLIENTS}w${WORKERS}"

echo "${TS},${SCENARIO},${WORKLOAD},${CLIENTS},${WORKERS},${TASKS},${COMPLETED},${AGG_SUBMIT_THROUGHPUT},${COMPLETION_THROUGHPUT},${E2E_P50},${E2E_P95},${QW_P50},${QW_P95},${CONNECTION_COUNT},${WALL_SECONDS}" >> "$OUTPUT_CSV"

echo "Results appended to $OUTPUT_CSV"
echo "===== Benchmark: PASS ====="
exit 0
