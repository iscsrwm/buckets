# Buckets Makefile
# High-performance S3-compatible object storage in C

# Compiler and flags
CC := gcc
CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -O2 -fPIC
LDFLAGS := -lssl -lcrypto -luuid -lz -lisal -lpthread -lm
INCLUDES := -Iinclude -Isrc -Ithird_party/cJSON

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
MIGRATION_SRC := $(wildcard $(SRC_DIR)/migration/*.c)
NET_SRC := $(wildcard $(SRC_DIR)/net/*.c)
S3_SRC := $(wildcard $(SRC_DIR)/s3/*.c)
ADMIN_SRC := $(wildcard $(SRC_DIR)/admin/*.c)
CJSON_SRC := third_party/cJSON/cJSON.c

ALL_SRC := $(CORE_SRC) $(CLUSTER_SRC) $(HASH_SRC) $(CRYPTO_SRC) $(ERASURE_SRC) \
           $(STORAGE_SRC) $(REGISTRY_SRC) $(TOPOLOGY_SRC) $(MIGRATION_SRC) \
           $(NET_SRC) $(S3_SRC) $(ADMIN_SRC) $(CJSON_SRC)

# Object files
CORE_OBJ := $(CORE_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CLUSTER_OBJ := $(CLUSTER_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
HASH_OBJ := $(HASH_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CRYPTO_OBJ := $(CRYPTO_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
ERASURE_OBJ := $(ERASURE_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
STORAGE_OBJ := $(STORAGE_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
REGISTRY_OBJ := $(REGISTRY_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
TOPOLOGY_OBJ := $(TOPOLOGY_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
MIGRATION_OBJ := $(MIGRATION_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
NET_OBJ := $(NET_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
S3_OBJ := $(S3_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
ADMIN_OBJ := $(ADMIN_SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
CJSON_OBJ := $(OBJ_DIR)/cJSON.o

ALL_OBJ := $(CORE_OBJ) $(CLUSTER_OBJ) $(HASH_OBJ) $(CRYPTO_OBJ) $(ERASURE_OBJ) \
           $(STORAGE_OBJ) $(REGISTRY_OBJ) $(TOPOLOGY_OBJ) $(MIGRATION_OBJ) \
           $(NET_OBJ) $(S3_OBJ) $(ADMIN_OBJ) $(CJSON_OBJ)

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
	@echo "  test-valgrind- Run tests with valgrind"
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
	@mkdir -p $(OBJ_DIR)/topology $(OBJ_DIR)/migration $(OBJ_DIR)/net
	@mkdir -p $(OBJ_DIR)/s3 $(OBJ_DIR)/admin

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
test: test-format test-topology test-endpoint test-erasure test-scanner test-worker

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

test-hash: $(TEST_BIN_DIR)/hash/test_runner
	@echo "Running hash tests..."
	@$<

test-crypto: $(TEST_BIN_DIR)/crypto/test_runner
	@echo "Running crypto tests..."
	@$<

test-erasure: $(TEST_BIN_DIR)/erasure/test_erasure
	@echo "Running erasure coding tests..."
	@$<

test-scanner: $(TEST_BIN_DIR)/migration/test_scanner
	@echo "Running migration scanner tests..."
	@$<

test-worker: $(TEST_BIN_DIR)/migration/test_worker
	@echo "Running migration worker tests..."
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

$(TEST_BIN_DIR)/migration/test_scanner: $(TEST_DIR)/migration/test_scanner.c $(BUILD_DIR)/libbuckets.a
	@mkdir -p $(dir $@)
	@echo "CC TEST $<"
	@$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(BUILD_DIR)/libbuckets.a $(LDFLAGS) -lcriterion

$(TEST_BIN_DIR)/migration/test_worker: $(TEST_DIR)/migration/test_worker.c $(BUILD_DIR)/libbuckets.a
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
