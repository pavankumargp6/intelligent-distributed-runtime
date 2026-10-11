# Intelligent Distributed Runtime — build and test targets

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -Isrc
BUILD    ?= build

# ---------------------------------------------------------------------------
# Source sets
# ---------------------------------------------------------------------------

COMMON_SRCS = \
    src/network_protocol.c \
    src/wire_fields.c

COORDINATOR_SRCS = \
    $(COMMON_SRCS) \
    src/runtime_server_main.c \
    src/runtime_server.c \
    src/runtime.c \
    src/worker_registry.c \
    src/task_manager.c \
    src/task_queue.c \
    src/scheduler.c \
    src/worker_pool.c \
    src/worker.c \
    src/worker_resources.c \
    src/process_info.c \
    src/cpu_monitor.c \
    src/task_executor.c \
    src/task_metrics.c \
    src/runtime_stats.c \
    src/worker_registration.c \
    src/coordinator_tasks.c \
    src/coordinator_handlers.c \
    src/connection_pool.c

WORKER_SRCS = \
    $(COMMON_SRCS) \
    src/worker_agent.c \
    src/worker.c \
    src/worker_resources.c \
    src/process_info.c \
    src/cpu_monitor.c \
    src/worker_registration.c \
    src/worker_registration_client.c \
    src/worker_task_client.c \
    src/task_executor.c \
    src/task_metrics.c

CLIENT_SUBMIT_SRCS = \
    $(COMMON_SRCS) \
    tests/client_submit.c

CLIENT_QUERY_SRCS = \
    src/network_protocol.c \
    tests/client_query.c

# ---------------------------------------------------------------------------
# Binaries
# ---------------------------------------------------------------------------

COORDINATOR_BIN = $(BUILD)/runtime_server
WORKER_BIN      = $(BUILD)/worker_agent
CLIENT_SUB_BIN  = $(BUILD)/client_submit
CLIENT_QRY_BIN  = $(BUILD)/client_query

# ASAN builds go into a separate directory so both can coexist.
ASAN_BUILD  = $(BUILD)/asan
ASAN_CFLAGS = $(CFLAGS) -g -O1 \
              -fsanitize=address,undefined \
              -fno-omit-frame-pointer

ASAN_COORDINATOR = $(ASAN_BUILD)/runtime_server
ASAN_WORKER      = $(ASAN_BUILD)/worker_agent
ASAN_CLIENT_SUB  = $(ASAN_BUILD)/client_submit
ASAN_CLIENT_QRY  = $(ASAN_BUILD)/client_query

# ---------------------------------------------------------------------------
# Phony targets
# ---------------------------------------------------------------------------

.PHONY: all asan test clean help

all: $(COORDINATOR_BIN) $(WORKER_BIN) $(CLIENT_SUB_BIN) $(CLIENT_QRY_BIN)
	@echo ""
	@echo "Build complete:"
	@echo "  $(COORDINATOR_BIN)"
	@echo "  $(WORKER_BIN)"
	@echo "  $(CLIENT_SUB_BIN)"
	@echo "  $(CLIENT_QRY_BIN)"

# ---------------------------------------------------------------------------
# Regular binaries
# ---------------------------------------------------------------------------

$(COORDINATOR_BIN): $(COORDINATOR_SRCS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(WORKER_BIN): $(WORKER_SRCS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(CLIENT_SUB_BIN): $(CLIENT_SUBMIT_SRCS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(CLIENT_QRY_BIN): $(CLIENT_QUERY_SRCS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

# ---------------------------------------------------------------------------
# ASAN binaries
# ---------------------------------------------------------------------------

asan: $(ASAN_COORDINATOR) $(ASAN_WORKER) $(ASAN_CLIENT_SUB) $(ASAN_CLIENT_QRY)
	@echo ""
	@echo "ASAN build complete: $(ASAN_BUILD)/"
	@echo "Run these binaries directly; sanitizer errors print to stderr."

$(ASAN_COORDINATOR): $(COORDINATOR_SRCS)
	@mkdir -p $(ASAN_BUILD)
	$(CC) $(ASAN_CFLAGS) $^ -o $@

$(ASAN_WORKER): $(WORKER_SRCS)
	@mkdir -p $(ASAN_BUILD)
	$(CC) $(ASAN_CFLAGS) $^ -o $@

$(ASAN_CLIENT_SUB): $(CLIENT_SUBMIT_SRCS)
	@mkdir -p $(ASAN_BUILD)
	$(CC) $(ASAN_CFLAGS) $^ -o $@

$(ASAN_CLIENT_QRY): $(CLIENT_QUERY_SRCS)
	@mkdir -p $(ASAN_BUILD)
	$(CC) $(ASAN_CFLAGS) $^ -o $@

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

test:
	@mkdir -p $(BUILD)
	@echo "===== connection pool unit test ====="
	$(CC) $(CFLAGS) tests/connection_pool_test.c \
	    src/connection_pool.c src/network_protocol.c \
	    -o $(BUILD)/connection_pool_test
	$(BUILD)/connection_pool_test
	@echo ""
	@echo "===== remote_exec_e2e_test.sh ====="
	bash tests/remote_exec_e2e_test.sh
	@echo ""
	@echo "===== fault_injection_test.sh ====="
	bash tests/fault_injection_test.sh
	@echo ""
	@echo "===== pull_model_test.sh ====="
	bash tests/pull_model_test.sh
	@echo ""
	@echo "===== hung_client_test.sh ====="
	bash tests/hung_client_test.sh
	@echo ""
	@echo "All tests passed."

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------

clean:
	rm -rf $(BUILD)

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------

help:
	@echo "Targets:"
	@echo "  make         build coordinator, worker, and clients into $(BUILD)/"
	@echo "  make asan    build with AddressSanitizer + UBSan into $(ASAN_BUILD)/"
	@echo "  make test    run unit and integration tests"
	@echo "  make clean   remove $(BUILD)/"
	@echo "  make help    this message"
