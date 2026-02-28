# Buckets Makefile
# High-performance S3-compatible object storage in C

# Compiler and flags
CC := gcc
# NOTE: Mongoose removed - now using libuv-based HTTP server
CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -Wall -Wextra -Werror -pedantic -O2 -fPIC
LDFLAGS := -lssl -lcrypto -luuid -lz -lisal -lpthread -lm -ldl -lrt
INCLUDES := -Iinclude -Isrc -Ithird_party/cJSON \
            -Ithird_party/libuv/include -Ithird_party/llhttp/include

# Debug flags
DEBUG_FLAGS := -g -O0 -DDEBUG -fsanitize=address -fsanitize=undefined
PROFILE_FLAGS := -pg -O2

# Directories
SRC_DIR := src
INC_DIR := include
TEST_DIR := tests
BUILD_DIR := build
BIN_DIR := bin
OBJ_DIR := $(BUILD_DIR)/obj
TEST_BIN_DIR := $(BUILD_DIR)/test

# Source files
CORE_SRC := $(wildcard $(SRC_DIR)/core/*.c)
CLUSTER_SRC := $(wildcard $(SRC_DIR)/cluster/*.c)
HASH_SRC := $(wildcard $(SRC_DIR)/hash/*.c)
CRYPTO_SRC := $(wildcard $(SRC_DIR)/crypto/*.c)
ERASURE_SRC := $(wildcard $(SRC_DIR)/erasure/*.c)
STORAGE_SRC := $(wildcard $(SRC_DIR)/storage/*.c)
REGISTRY_SRC := $(wildcard $(SRC_DIR)/registry/*.c)
TOPOLOGY_SRC := $(wildcard $(SRC_DIR)/topology/*.c)
PLACEMENT_SRC := $(wildcard $(SRC_DIR)/placement/*.c)
MIGRATION_SRC := $(wildcard $(SRC_DIR)/migration/*.c)
NET_SRC := $(wildcard $(SRC_DIR)/net/*.c)
S3_SRC := $(wildcard $(SRC_DIR)/s3/*.c)
ADMIN_SRC := $(wildcard $(SRC_DIR)/admin/*.c)
CONFIG_SRC := $(wildcard $(SRC_DIR)/config/*.c)
CJSON_SRC := third_party/cJSON/cJSON.c
# MONGOOSE_SRC removed - using libuv-based http_server_uv.c instead

# libuv sources (Linux)
LIBUV_DIR := third_party/libuv
LIBUV_SRC := $(LIBUV_DIR)/src/fs-poll.c \
             $(LIBUV_DIR)/src/idna.c \
             $(LIBUV_DIR)/src/inet.c \
             $(LIBUV_DIR)/src/random.c \
             $(LIBUV_DIR)/src/strscpy.c \
             $(LIBUV_DIR)/src/strtok.c \
             $(LIBUV_DIR)/src/thread-common.c \
             $(LIBUV_DIR)/src/threadpool.c \
             $(LIBUV_DIR)/src/timer.c \
             $(LIBUV_DIR)/src/uv-common.c \
             $(LIBUV_DIR)/src/uv-data-getter-setters.c \
             $(LIBUV_DIR)/src/version.c \
             $(LIBUV_DIR)/src/unix/async.c \
             $(LIBUV_DIR)/src/unix/core.c \
             $(LIBUV_DIR)/src/unix/dl.c \
             $(LIBUV_DIR)/src/unix/fs.c \
             $(LIBUV_DIR)/src/unix/getaddrinfo.c \
             $(LIBUV_DIR)/src/unix/getnameinfo.c \
             $(LIBUV_DIR)/src/unix/loop-watcher.c \
             $(LIBUV_DIR)/src/unix/loop.c \
             $(LIBUV_DIR)/src/unix/pipe.c \
             $(LIBUV_DIR)/src/unix/poll.c \
             $(LIBUV_DIR)/src/unix/process.c \
             $(LIBUV_DIR)/src/unix/random-devurandom.c \
             $(LIBUV_DIR)/src/unix/signal.c \
             $(LIBUV_DIR)/src/unix/stream.c \
             $(LIBUV_DIR)/src/unix/tcp.c \
             $(LIBUV_DIR)/src/unix/thread.c \
             $(LIBUV_DIR)/src/unix/tty.c \
             $(LIBUV_DIR)/src/unix/udp.c \
             $(LIBUV_DIR)/src/unix/linux.c \
             $(LIBUV_DIR)/src/unix/procfs-exepath.c \
             $(LIBUV_DIR)/src/unix/proctitle.c \
             $(LIBUV_DIR)/src/unix/random-getrandom.c \
             $(LIBUV_DIR)/src/unix/random-sysctl-linux.c

# llhttp sources
LLHTTP_DIR := third_party/llhttp
LLHTTP_SRC := $(LLHTTP_DIR)/src/api.c \
              $(LLHTTP_DIR)/src/http.c \
              $(LLHTTP_DIR)/src/llhttp.c

ALL_SRC := $(CORE_SRC) $(CLUSTER_SRC) $(HASH_SRC) $(CRYPTO_SRC) $(ERASURE_SRC) \
           $(STORAGE_SRC) $(REGISTRY_SRC) $(TOPOLOGY_SRC) $(PLACEMENT_SRC) $(MIGRATION_SRC) \
           $(NET_SRC) $(S3_SRC) $(ADMIN_SRC) $(CONFIG_SRC) $(CJSON_SRC) \
           $(LIBUV_SRC) $(LLHTTP_SRC)

# Object files
CORE_OBJ := $(CORE_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CLUSTER_OBJ := $(CLUSTER_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
HASH_OBJ := $(HASH_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CRYPTO_OBJ := $(CRYPTO_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
ERASURE_OBJ := $(ERASURE_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
STORAGE_OBJ := $(STORAGE_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
REGISTRY_OBJ := $(REGISTRY_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
TOPOLOGY_OBJ := $(TOPOLOGY_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
PLACEMENT_OBJ := $(PLACEMENT_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
MIGRATION_OBJ := $(MIGRATION_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
NET_OBJ := $(NET_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
S3_OBJ := $(S3_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
ADMIN_OBJ := $(ADMIN_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CONFIG_OBJ := $(CONFIG_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CJSON_OBJ := $(OBJ_DIR)/cJSON.o
# MONGOOSE_OBJ removed - using libuv-based HTTP server

# libuv object files
LIBUV_OBJ := $(OBJ_DIR)/libuv/fs-poll.o \
             $(OBJ_DIR)/libuv/idna.o \
             $(OBJ_DIR)/libuv/inet.o \
             $(OBJ_DIR)/libuv/random.o \
             $(OBJ_DIR)/libuv/strscpy.o \
             $(OBJ_DIR)/libuv/strtok.o \
             $(OBJ_DIR)/libuv/thread-common.o \
             $(OBJ_DIR)/libuv/threadpool.o \
             $(OBJ_DIR)/libuv/timer.o \
             $(OBJ_DIR)/libuv/uv-common.o \
             $(OBJ_DIR)/libuv/uv-data-getter-setters.o \
             $(OBJ_DIR)/libuv/version.o \
             $(OBJ_DIR)/libuv/unix/async.o \
             $(OBJ_DIR)/libuv/unix/core.o \
             $(OBJ_DIR)/libuv/unix/dl.o \
             $(OBJ_DIR)/libuv/unix/fs.o \
             $(OBJ_DIR)/libuv/unix/getaddrinfo.o \
             $(OBJ_DIR)/libuv/unix/getnameinfo.o \
             $(OBJ_DIR)/libuv/unix/loop-watcher.o \
             $(OBJ_DIR)/libuv/unix/loop.o \
             $(OBJ_DIR)/libuv/unix/pipe.o \
             $(OBJ_DIR)/libuv/unix/poll.o \
             $(OBJ_DIR)/libuv/unix/process.o \
             $(OBJ_DIR)/libuv/unix/random-devurandom.o \
             $(OBJ_DIR)/libuv/unix/signal.o \
             $(OBJ_DIR)/libuv/unix/stream.o \
             $(OBJ_DIR)/libuv/unix/tcp.o \
             $(OBJ_DIR)/libuv/unix/thread.o \
             $(OBJ_DIR)/libuv/unix/tty.o \
             $(OBJ_DIR)/libuv/unix/udp.o \
             $(OBJ_DIR)/libuv/unix/linux.o \
             $(OBJ_DIR)/libuv/unix/procfs-exepath.o \
             $(OBJ_DIR)/libuv/unix/proctitle.o \
             $(OBJ_DIR)/libuv/unix/random-getrandom.o \
             $(OBJ_DIR)/libuv/unix/random-sysctl-linux.o

# llhttp object files
LLHTTP_OBJ := $(OBJ_DIR)/llhttp/api.o \
              $(OBJ_DIR)/llhttp/http.o \
              $(OBJ_DIR)/llhttp/llhttp.o

ALL_OBJ := $(CORE_OBJ) $(CLUSTER_OBJ) $(HASH_OBJ) $(CRYPTO_OBJ) $(ERASURE_OBJ) \
           $(STORAGE_OBJ) $(REGISTRY_OBJ) $(TOPOLOGY_OBJ) $(PLACEMENT_OBJ) $(MIGRATION_OBJ) \
           $(NET_OBJ) $(S3_OBJ) $(ADMIN_OBJ) $(CONFIG_OBJ) $(CJSON_OBJ) \
           $(LIBUV_OBJ) $(LLHTTP_OBJ)

# Test files
TEST_SRC := $(wildcard $(TEST_DIR)/**/*.c)
TEST_BIN := $(TEST_SRC:$(TEST_DIR)/%.c=$(TEST_BIN_DIR)/%)

# Benchmark files
BENCH_DIR := benchmarks
BENCH_SRC := $(wildcard $(BENCH_DIR)/*.c)
BENCH_BIN := $(BENCH_SRC:$(BENCH_DIR)/%.c=$(BIN_DIR)/%)

# Targets
.PHONY: all clean test install debug profile help benchmark

all: directories libbuckets buckets

help:
	@echo "Buckets Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build everything (default)"
	@echo "  libbuckets   - Build core library"
	@echo "  buckets      - Build server binary"
	@echo "  benchmark    - Build and run performance benchmarks"
	@echo "  test         - Run all tests"
	@echo "  test-core    - Test core components"
	@echo "  test-hash    - Test hashing"
	@echo "  test-scanner - Test migration scanner"
	@echo "  test-worker  - Test migration workers"
	@echo "  test-orchestrator - Test migration orchestrator"
	@echo "  test-throttle - Test migration throttle"
	@echo "  test-checkpoint - Test migration checkpoint"
	@echo "  test-rpc      - Test RPC message format"
	@echo "  test-broadcast- Test RPC broadcast"
	@echo "  test-valgrind - Run tests with valgrind"
	@echo "  debug        - Build with debug symbols"
	@echo "  profile      - Build with profiling"
	@echo "  clean        - Remove build artifacts"
	@echo "  install      - Install to system"
	@echo ""
	@echo "Variables:"
	@echo "  DEBUG=1      - Enable debug build"
	@echo "  VERBOSE=1    - Verbose output"

# Create directories
directories:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR) $(OBJ_DIR) $(TEST_BIN_DIR)
	@mkdir -p $(OBJ_DIR)/core $(OBJ_DIR)/cluster $(OBJ_DIR)/hash $(OBJ_DIR)/crypto
	@mkdir -p $(OBJ_DIR)/erasure $(OBJ_DIR)/storage $(OBJ_DIR)/registry
	@mkdir -p $(OBJ_DIR)/topology $(OBJ_DIR)/placement $(OBJ_DIR)/migration $(OBJ_DIR)/net
	@mkdir -p $(OBJ_DIR)/s3 $(OBJ_DIR)/admin $(OBJ_DIR)/config
	@mkdir -p $(OBJ_DIR)/libuv $(OBJ_DIR)/libuv/unix $(OBJ_DIR)/llhttp
	@mkdir -p $(TEST_BIN_DIR)/net $(TEST_BIN_DIR)/migration $(TEST_BIN_DIR)/s3

# Library target
libbuckets: $(BUILD_DIR)/libbuckets.a $(BUILD_DIR)/libbuckets.so

$(BUILD_DIR)/libbuckets.a: $(ALL_OBJ)
	@echo "AR $@"
	@ar rcs $@ $^

$(BUILD_DIR)/libbuckets.so: $(ALL_OBJ)
	@echo "LD $@"
	@$(CC) -shared -o $@ $^ $(LDFLAGS)

# Server binary
buckets: $(BIN_DIR)/buckets

$(BIN_DIR)/buckets: $(SRC_DIR)/main.c $(BUILD_DIR)/libbuckets.a
	@echo "CC $@"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS)

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "CC $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile cJSON
$(OBJ_DIR)/cJSON.o: third_party/cJSON/cJSON.c
	@echo "CC $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Mongoose removed - using libuv-based HTTP server (http_server_uv.c)

# Compile libuv (relaxed warnings for third-party code)
LIBUV_CFLAGS := -std=c11 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200112 -O2 -fPIC -fvisibility=hidden \
                -Wno-unused-parameter -Wno-sign-compare \
                -Ithird_party/libuv/include -Ithird_party/libuv/src

$(OBJ_DIR)/libuv/%.o: third_party/libuv/src/%.c
	@echo "CC $<"
	@$(CC) $(LIBUV_CFLAGS) -c $< -o $@

$(OBJ_DIR)/libuv/unix/%.o: third_party/libuv/src/unix/%.c
	@echo "CC $<"
	@$(CC) $(LIBUV_CFLAGS) -c $< -o $@

# Compile llhttp (relaxed warnings for third-party code)
LLHTTP_CFLAGS := -std=c11 -O2 -fPIC -Wno-unused-parameter \
                 -Ithird_party/llhttp/include

$(OBJ_DIR)/llhttp/%.o: third_party/llhttp/src/%.c
	@echo "CC $<"
	@$(CC) $(LLHTTP_CFLAGS) -c $< -o $@

# Component-specific builds
.PHONY: core cluster hash crypto erasure storage registry topology migration net s3 admin

core: $(CORE_OBJ)
cluster: $(CLUSTER_OBJ)
hash: $(HASH_OBJ)
crypto: $(CRYPTO_OBJ)
erasure: $(ERASURE_OBJ)
storage: $(STORAGE_OBJ)
registry: $(REGISTRY_OBJ)
topology: $(TOPOLOGY_OBJ)
migration: $(MIGRATION_OBJ)
net: $(NET_OBJ)
s3: $(S3_OBJ)
admin: $(ADMIN_OBJ)

# Tests
test: test-format test-topology test-endpoint test-erasure test-scanner test-worker test-orchestrator test-throttle test-checkpoint test-http-server test-router test-conn-pool test-peer-grid test-rpc test-broadcast test-s3-xml test-s3-ops test-s3-buckets

test-format: $(TEST_BIN_DIR)/cluster/test_format
	@echo "Running format tests..."
	@$<

test-topology: $(TEST_BIN_DIR)/cluster/test_topology
	@echo "Running topology tests..."
	@$<

test-topology-operations: $(TEST_BIN_DIR)/cluster/test_topology_operations
	@echo "Running topology operations tests..."
	@$<

test-topology-quorum: $(TEST_BIN_DIR)/cluster/test_topology_quorum
	@echo "Running topology quorum tests..."
	@$<

test-topology-manager: $(TEST_BIN_DIR)/cluster/test_topology_manager
	@echo "Running topology manager tests..."
	@$<

test-topology-integration: $(TEST_BIN_DIR)/cluster/test_topology_integration
	@echo "Running topology integration tests..."
	@$<

test-endpoint: $(TEST_BIN_DIR)/cluster/test_endpoint
	@echo "Running endpoint tests..."
	@$<

test-core: $(TEST_BIN_DIR)/core/test_runner
	@echo "Running core tests..."
	@$<

test-hash: $(TEST_BIN_DIR)/hash/test_siphash $(TEST_BIN_DIR)/hash/test_xxhash $(TEST_BIN_DIR)/hash/test_ring
	@echo "Running hash tests..."
	@$(TEST_BIN_DIR)/hash/test_siphash
	@$(TEST_BIN_DIR)/hash/test_xxhash
	@$(TEST_BIN_DIR)/hash/test_ring

test-crypto: $(TEST_BIN_DIR)/crypto/test_blake2b $(TEST_BIN_DIR)/crypto/test_sha256
	@echo "Running crypto tests..."
	@$(TEST_BIN_DIR)/crypto/test_blake2b
	@$(TEST_BIN_DIR)/crypto/test_sha256

test-erasure: $(TEST_BIN_DIR)/erasure/test_erasure
	@echo "Running erasure coding tests..."
	@$<

test-storage: $(TEST_BIN_DIR)/storage/test_object
	@echo "Running storage tests..."
	@$(TEST_BIN_DIR)/storage/test_object

test-scanner: $(TEST_BIN_DIR)/migration/test_scanner
	@echo "Running migration scanner tests..."
	@$<

test-worker: $(TEST_BIN_DIR)/migration/test_worker
	@echo "Running migration worker tests..."
	@$<

test-orchestrator: $(TEST_BIN_DIR)/migration/test_orchestrator
	@echo "Running migration orchestrator tests..."
	@$<

test-throttle: $(TEST_BIN_DIR)/migration/test_throttle
	@echo "Running migration throttle tests..."
	@$<

test-checkpoint: $(TEST_BIN_DIR)/migration/test_checkpoint
	@echo "Running migration checkpoint tests..."
	@$<

test-integration: $(TEST_BIN_DIR)/migration/test_integration
	@echo "Running migration integration tests..."
	@$<

test-http-server: $(TEST_BIN_DIR)/net/test_http_server
	@echo "Running HTTP server tests..."
	@$<

test-router: $(TEST_BIN_DIR)/net/test_router
	@echo "Running router tests..."
	@$<

test-conn-pool: $(TEST_BIN_DIR)/net/test_conn_pool
	@echo "Running connection pool tests..."
	@$<

test-peer-grid: $(TEST_BIN_DIR)/net/test_peer_grid
	@echo "Running peer grid tests..."
	@$<

test-rpc: $(TEST_BIN_DIR)/net/test_rpc
	@echo "Running RPC tests..."
	@$<

test-broadcast: $(TEST_BIN_DIR)/net/test_broadcast
	@echo "Running broadcast tests..."
	@$<

test-s3-xml: $(TEST_BIN_DIR)/s3/test_s3_xml
	@echo "Running S3 XML tests..."
	@$<

test-s3-ops: $(TEST_BIN_DIR)/s3/test_s3_ops
	@echo "Running S3 operations tests..."
	@$<

test-s3-buckets: $(TEST_BIN_DIR)/s3/test_s3_buckets
	@echo "Running S3 bucket operations tests..."
	@$<

test-s3-multipart: $(TEST_BIN_DIR)/s3/test_s3_multipart
	@echo "Running S3 multipart upload tests..."
	@$<

# Test binaries (Criterion-based tests)
$(TEST_BIN_DIR)/cluster/test_format: $(TEST_DIR)/cluster/test_format.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/cluster/test_topology: $(TEST_DIR)/cluster/test_topology.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/cluster/test_topology_operations: $(TEST_DIR)/cluster/test_topology_operations.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/cluster/test_topology_quorum: $(TEST_DIR)/cluster/test_topology_quorum.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/cluster/test_topology_manager: $(TEST_DIR)/cluster/test_topology_manager.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/cluster/test_topology_integration: $(TEST_DIR)/cluster/test_topology_integration.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/erasure/test_erasure: $(TEST_DIR)/erasure/test_erasure.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lisal -lcriterion

$(TEST_BIN_DIR)/hash/test_siphash: $(TEST_DIR)/hash/test_siphash.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/hash/test_xxhash: $(TEST_DIR)/hash/test_xxhash.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/hash/test_ring: $(TEST_DIR)/hash/test_ring.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/crypto/test_blake2b: $(TEST_DIR)/crypto/test_blake2b.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/crypto/test_sha256: $(TEST_DIR)/crypto/test_sha256.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/storage/test_object: $(TEST_DIR)/storage/test_object.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lisal -lcriterion

$(TEST_BIN_DIR)/migration/test_scanner: $(TEST_DIR)/migration/test_scanner.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/migration/test_worker: $(TEST_DIR)/migration/test_worker.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/migration/test_orchestrator: $(TEST_DIR)/migration/test_orchestrator.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/migration/test_throttle: $(TEST_DIR)/migration/test_throttle.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/migration/test_checkpoint: $(TEST_DIR)/migration/test_checkpoint.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/migration/test_integration: $(TEST_DIR)/migration/test_integration.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/net/test_http_server: $(TEST_DIR)/net/test_http_server.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/net/test_router: $(TEST_DIR)/net/test_router.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/net/test_conn_pool: $(TEST_DIR)/net/test_conn_pool.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/net/test_peer_grid: $(TEST_DIR)/net/test_peer_grid.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/net/test_rpc: $(TEST_DIR)/net/test_rpc.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/net/test_broadcast: $(TEST_DIR)/net/test_broadcast.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/s3/test_s3_xml: $(TEST_DIR)/s3/test_s3_xml.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/s3/test_s3_ops: $(TEST_DIR)/s3/test_s3_ops.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/s3/test_s3_buckets: $(TEST_DIR)/s3/test_s3_buckets.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/s3/test_s3_multipart: $(TEST_DIR)/s3/test_s3_multipart.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

# Generic test binary rule
$(TEST_BIN_DIR)/%: $(TEST_DIR)/%.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< -L$(BUILD_DIR) -lbuckets $(LDFLAGS)

# Valgrind testing
test-valgrind: $(TEST_BIN)
	@echo "Running tests with valgrind..."
	@for test in $(TEST_BIN); do \
		echo "Testing $$test"; \
		valgrind --leak-check=full --error-exitcode=1 $$test; \
	done

# Debug build
debug:
	$(MAKE) CFLAGS="$(CFLAGS) $(DEBUG_FLAGS)"

# Profile build
profile:
	$(MAKE) CFLAGS="$(CFLAGS) $(PROFILE_FLAGS)"

# Install
PREFIX ?= /usr/local
install: all
	@echo "Installing to $(PREFIX)..."
	@install -d $(PREFIX)/bin
	@install -d $(PREFIX)/lib
	@install -d $(PREFIX)/include/buckets
	@install -m 755 $(BIN_DIR)/buckets $(PREFIX)/bin/
	@install -m 644 $(BUILD_DIR)/libbuckets.a $(PREFIX)/lib/
	@install -m 755 $(BUILD_DIR)/libbuckets.so $(PREFIX)/lib/
	@cp -r $(INC_DIR)/* $(PREFIX)/include/buckets/

# Benchmark target
benchmark: $(BUILD_DIR)/libbuckets.a
	@echo "Building Phase 4 benchmarks..."
	@mkdir -p $(BIN_DIR)
	@$(CC) $(CFLAGS) $(INCLUDES) -o $(BIN_DIR)/bench_phase4 \
		$(BENCH_DIR)/bench_phase4.c $(BUILD_DIR)/libbuckets.a $(LDFLAGS)
	@echo ""
	@echo "Running benchmarks..."
	@$(BIN_DIR)/bench_phase4

# Clean
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(BIN_DIR)

# Formatting
.PHONY: format
format:
	@echo "Formatting code..."
	@find src include tests -name "*.c" -o -name "*.h" | xargs clang-format -i

# Static analysis
.PHONY: analyze
analyze:
	@echo "Running static analysis..."
	@clang-tidy $(ALL_SRC) -- $(INCLUDES)

# Dependencies
-include $(ALL_OBJ:.o=.d)

# Pattern rules for dependency generation
$(OBJ_DIR)/%.d: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(INCLUDES) -MM -MT $(OBJ_DIR)/$*.o $< > $@
