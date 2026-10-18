# Intelligent Distributed Runtime — build and test targets

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -Isrc
BUILD    ?= build

COMMON_SRCS = \
    src/network_protocol.c \
    src/wire_fields.c \
    src/sha256.c \
    src/hmac.c \
    src/auth.c \
    src/auth_config.c

COORDINATOR_SRCS = \
    src/log.c \
    src/wal.c \
    src/recovery.c \
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
    src/log.c \
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
    $(COMMON_SRCS) \
    tests/client_query.c

CLIENT_STATUS_SRCS = \
    $(COMMON_SRCS) \
    tests/client_status.c

COORDINATOR_BIN   = $(BUILD)/runtime_server
WORKER_BIN        = $(BUILD)/worker_agent
CLIENT_SUB_BIN    = $(BUILD)/client_submit
CLIENT_QRY_BIN    = $(BUILD)/client_query
CLIENT_STATUS_BIN = $(BUILD)/client_status

ASAN_BUILD  = $(BUILD)/asan
ASAN_CFLAGS = $(CFLAGS) -g -O1 \
	      -fsanitize=address,undefined \
	      -fno-omit-frame-pointer

ASAN_COORDINATOR   = $(ASAN_BUILD)/runtime_server
ASAN_WORKER        = $(ASAN_BUILD)/worker_agent
ASAN_CLIENT_SUB    = $(ASAN_BUILD)/client_submit
ASAN_CLIENT_QRY    = $(ASAN_BUILD)/client_query
ASAN_CLIENT_STATUS = $(ASAN_BUILD)/client_status

UBSAN_BUILD  = $(BUILD)/ubsan
UBSAN_CFLAGS = $(CFLAGS) -g -O1 \
	       -fsanitize=undefined \
	       -fno-omit-frame-pointer

UBSAN_COORDINATOR   = $(UBSAN_BUILD)/runtime_server
UBSAN_WORKER        = $(UBSAN_BUILD)/worker_agent
UBSAN_CLIENT_SUB    = $(UBSAN_BUILD)/client_submit
UBSAN_CLIENT_QRY    = $(UBSAN_BUILD)/client_query
UBSAN_CLIENT_STATUS = $(UBSAN_BUILD)/client_status

ANALYZER_BUILD  = $(BUILD)/analyzer
ANALYZER_CFLAGS = $(CFLAGS) -g -O0 -fanalyzer

ANALYZER_COORDINATOR   = $(ANALYZER_BUILD)/runtime_server
ANALYZER_WORKER        = $(ANALYZER_BUILD)/worker_agent
ANALYZER_CLIENT_SUB    = $(ANALYZER_BUILD)/client_submit
ANALYZER_CLIENT_QRY    = $(ANALYZER_BUILD)/client_query
ANALYZER_CLIENT_STATUS = $(ANALYZER_BUILD)/client_status

.PHONY: all asan ubsan analyzer test clean help

all: $(COORDINATOR_BIN) $(WORKER_BIN) \
     $(CLIENT_SUB_BIN) $(CLIENT_QRY_BIN) $(CLIENT_STATUS_BIN) \
     $(BUILD)/raw_submit
	@echo ""
	@echo "Build complete:"
	@echo "  $(COORDINATOR_BIN)"
	@echo "  $(WORKER_BIN)"
	@echo "  $(CLIENT_SUB_BIN)"
	@echo "  $(CLIENT_QRY_BIN)"
	@echo "  $(CLIENT_STATUS_BIN)"

$(BUILD)/raw_submit: tests/raw_submit.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) tests/raw_submit.c $(COMMON_SRCS) -o $@

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

$(CLIENT_STATUS_BIN): $(CLIENT_STATUS_SRCS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

asan: $(ASAN_COORDINATOR) $(ASAN_WORKER) \
      $(ASAN_CLIENT_SUB) $(ASAN_CLIENT_QRY) $(ASAN_CLIENT_STATUS)
	@echo ""
	@echo "ASAN build complete: $(ASAN_BUILD)/"

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

$(ASAN_CLIENT_STATUS): $(CLIENT_STATUS_SRCS)
	@mkdir -p $(ASAN_BUILD)
	$(CC) $(ASAN_CFLAGS) $^ -o $@

ubsan: $(UBSAN_COORDINATOR) $(UBSAN_WORKER) \
       $(UBSAN_CLIENT_SUB) $(UBSAN_CLIENT_QRY) $(UBSAN_CLIENT_STATUS)
	@echo ""
	@echo "UBSan build complete: $(UBSAN_BUILD)/"
	@echo "Run these binaries directly; UBSan errors print to stderr."

$(UBSAN_COORDINATOR): $(COORDINATOR_SRCS)
	@mkdir -p $(UBSAN_BUILD)
	$(CC) $(UBSAN_CFLAGS) $^ -o $@

$(UBSAN_WORKER): $(WORKER_SRCS)
	@mkdir -p $(UBSAN_BUILD)
	$(CC) $(UBSAN_CFLAGS) $^ -o $@

$(UBSAN_CLIENT_SUB): $(CLIENT_SUBMIT_SRCS)
	@mkdir -p $(UBSAN_BUILD)
	$(CC) $(UBSAN_CFLAGS) $^ -o $@

$(UBSAN_CLIENT_QRY): $(CLIENT_QUERY_SRCS)
	@mkdir -p $(UBSAN_BUILD)
	$(CC) $(UBSAN_CFLAGS) $^ -o $@

$(UBSAN_CLIENT_STATUS): $(CLIENT_STATUS_SRCS)
	@mkdir -p $(UBSAN_BUILD)
	$(CC) $(UBSAN_CFLAGS) $^ -o $@

analyzer: $(ANALYZER_COORDINATOR) $(ANALYZER_WORKER) \
	  $(ANALYZER_CLIENT_SUB) $(ANALYZER_CLIENT_QRY) $(ANALYZER_CLIENT_STATUS)
	@echo ""
	@echo "Static analysis complete: $(ANALYZER_BUILD)/"
	@echo "If no diagnostics appeared above, the analysis found nothing."
	@echo "The binaries here are not intended to be run; see make asan or make ubsan."

$(ANALYZER_COORDINATOR): $(COORDINATOR_SRCS)
	@mkdir -p $(ANALYZER_BUILD)
	$(CC) $(ANALYZER_CFLAGS) $^ -o $@

$(ANALYZER_WORKER): $(WORKER_SRCS)
	@mkdir -p $(ANALYZER_BUILD)
	$(CC) $(ANALYZER_CFLAGS) $^ -o $@

$(ANALYZER_CLIENT_SUB): $(CLIENT_SUBMIT_SRCS)
	@mkdir -p $(ANALYZER_BUILD)
	$(CC) $(ANALYZER_CFLAGS) $^ -o $@

$(ANALYZER_CLIENT_QRY): $(CLIENT_QUERY_SRCS)
	@mkdir -p $(ANALYZER_BUILD)
	$(CC) $(ANALYZER_CFLAGS) $^ -o $@

$(ANALYZER_CLIENT_STATUS): $(CLIENT_STATUS_SRCS)
	@mkdir -p $(ANALYZER_BUILD)
	$(CC) $(ANALYZER_CFLAGS) $^ -o $@

test:
	@mkdir -p $(BUILD)
	@echo "===== connection pool unit test ====="
	$(CC) $(CFLAGS) tests/connection_pool_test.c \
	    src/connection_pool.c src/network_protocol.c src/log.c \
	    src/sha256.c src/hmac.c src/auth.c src/auth_config.c \
	    -o $(BUILD)/connection_pool_test
	$(BUILD)/connection_pool_test
	@echo ""
	@echo "===== log_test.sh ====="
	bash tests/log_test.sh
	@echo ""
	@echo "===== sha256_test.sh ====="
	bash tests/sha256_test.sh
	@echo ""
	@echo "===== hmac_test.sh ====="
	bash tests/hmac_test.sh
	@echo ""
	@echo "===== auth_test.sh ====="
	bash tests/auth_test.sh
	@echo ""
	@echo "===== auth_replay_test.sh ====="
	bash tests/auth_replay_test.sh
	@echo ""
	@echo "===== auth_wrong_role_test.sh ====="
	bash tests/auth_wrong_role_test.sh
	@echo ""
	@echo "===== auth_missing_secret_test.sh ====="
	bash tests/auth_missing_secret_test.sh
	@echo ""
	@echo "===== auth_insecure_misuse_test.sh ====="
	bash tests/auth_insecure_misuse_test.sh
	@echo ""
	@echo "===== wal_test.sh ====="
	bash tests/wal_test.sh
	@echo ""
	@echo "===== recovery_compact_test.sh ====="
	bash tests/recovery_compact_test.sh
	@echo ""
	@echo ""
	@echo "===== raw_submit_test.sh ====="
	bash tests/raw_submit_test.sh
	@echo ""
	@echo "===== worker_reincarnation_test.sh ====="
	bash tests/worker_reincarnation_test.sh
	@echo "===== wal_meta_recovery_test.sh ====="
	bash tests/wal_meta_recovery_test.sh
	@echo ""
	@echo "===== wal_overflow_test.sh ====="
	bash tests/wal_overflow_test.sh
	@echo ""
	@echo "===== worker_resources_test.sh ====="
	bash tests/worker_resources_test.sh
	@echo ""
	@echo "===== cpu_monitor_test.sh ====="
	bash tests/cpu_monitor_test.sh
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
	@echo "===== status_test.sh ====="
	bash tests/status_test.sh
	@echo ""
	@echo "===== dispatch_timeout_test.sh ====="
	bash tests/dispatch_timeout_test.sh
	@echo ""
	@echo "===== wal_recovery_test.sh ====="
	bash tests/wal_recovery_test.sh
	@echo ""
	@echo "All tests passed."

clean:
	rm -rf $(BUILD)

help:
	@echo "Targets:"
	@echo "  make         build coordinator, worker, and three clients into $(BUILD)/"
	@echo "  make asan    build with AddressSanitizer + UBSan into $(ASAN_BUILD)/"
	@echo "  make ubsan   build with UndefinedBehaviorSanitizer into $(UBSAN_BUILD)/"
	@echo "  make analyzer  build with GCC -fanalyzer into $(ANALYZER_BUILD)/"
	@echo "  make test    run unit and integration tests"
	@echo "  make clean   remove $(BUILD)/"
	@echo "  make help    this message"


