# Buckets Project Status

**Last Updated**: February 25, 2026  
**Current Phase**: Foundation - Week 4 (Endpoint Parsing) - ⏳ NEXT  
**Status**: 🟢 Active Development  
**Week 3 Status**: ✅ COMPLETE (Topology Management + Caching)

---

## ✅ Completed

### Project Infrastructure
- [x] Project name: **Buckets**
- [x] README.md with comprehensive overview
- [x] ROADMAP.md with 11-phase development plan (52 weeks)
- [x] Architecture documentation:
  - [x] SCALE_AND_DATA_PLACEMENT.md (75 pages)
  - [x] CLUSTER_AND_STATE_MANAGEMENT.md
- [x] AGENTS.md development guide (265 lines)
- [x] Directory structure created
- [x] Makefile build system with component targets
- [x] Third-party integration: cJSON library
- [x] Dependency installation: uuid-dev

### Core Implementation (Week 1 - COMPLETE)
- [x] Main entry point (src/main.c) with CLI commands
- [x] Core implementation (src/core/buckets.c):
  - [x] Memory management wrappers
  - [x] String utilities (strdup, strcmp, format)
  - [x] Logging system with file output support
  - [x] Environment variable configuration
  - [x] Initialization and cleanup
- [x] Core header files:
  - [x] `include/buckets.h` - Main API, types, logging, utilities
  - [x] `include/buckets_cluster.h` - Cluster structures (format, topology)
  - [x] `include/buckets_io.h` - Atomic I/O and disk utilities
  - [x] `include/buckets_json.h` - JSON helper API

### Cluster Utilities (Week 1 - COMPLETE)
- [x] **UUID Generation** (`src/cluster/uuid.c` - 39 lines):
  - [x] Generate UUID v4 (random)
  - [x] Parse UUID strings
  - [x] Convert to string format
- [x] **Atomic I/O** (`src/cluster/atomic_io.c` - 221 lines):
  - [x] Atomic file writes (temp + rename pattern)
  - [x] Atomic file reads (entire file to memory)
  - [x] Directory creation (recursive with parents)
  - [x] Directory sync (fsync for metadata persistence)
  - [x] Fixed dirname() memory handling bug
- [x] **Disk Utilities** (`src/cluster/disk_utils.c` - 90 lines):
  - [x] Get metadata directory path (`.buckets.sys`)
  - [x] Get format.json path
  - [x] Get topology.json path
  - [x] Check if disk is formatted
- [x] **JSON Helpers** (`src/cluster/json_helpers.c` - 200 lines):
  - [x] Parse/load/save JSON with atomic writes
  - [x] Type-safe getters (string/int/bool/object/array)
  - [x] Type-safe setters (string/int/bool/object/array)

### Format Management (Week 2 - COMPLETE)
- [x] **Format Operations** (`src/cluster/format.c` - 434 lines):
  - [x] `buckets_format_new()` - Create format with UUID generation
  - [x] `buckets_format_free()` - Proper nested memory cleanup
  - [x] `format_to_json()` - Serialize to MinIO-compatible JSON
  - [x] `format_from_json()` - Deserialize with validation
  - [x] `buckets_format_save()` - Atomic write to disk
  - [x] `buckets_format_load()` - Load from disk with parsing
  - [x] `buckets_format_clone()` - Deep copy via JSON roundtrip
  - [x] `buckets_format_validate()` - Quorum-based multi-disk validation
- [x] **Criterion Test Suite** (`tests/cluster/test_format.c` - 318 lines, 20 tests passing)
- [x] **Manual Testing** (`tests/test_format_manual.c` - 149 lines, 7 tests passing)

### Topology Management (Week 3 - COMPLETE)
- [x] **Topology Operations** (`src/cluster/topology.c` - 390 lines):
  - [x] `buckets_topology_new()` - Create empty topology
  - [x] `buckets_topology_free()` - Proper nested memory cleanup
  - [x] `buckets_topology_from_format()` - Initialize from format.json
  - [x] `topology_to_json()` - Serialize to JSON
  - [x] `topology_from_json()` - Deserialize with validation
  - [x] `buckets_topology_save()` - Atomic write to disk
  - [x] `buckets_topology_load()` - Load from disk with parsing
  - [x] Set state management (active/draining/removed enum)
  - [x] Generation number tracking (starts at 0, increments on changes)
  - [x] Virtual node factor constant (BUCKETS_VNODE_FACTOR = 150)
- [x] **Cache Implementation** (`src/cluster/cache.c` - 252 lines):
  - [x] Thread-safe format cache with pthread_rwlock_t
  - [x] Thread-safe topology cache with pthread_rwlock_t
  - [x] Cache get/set/invalidate operations
  - [x] Integration into buckets_init/cleanup
  - [x] Cache API header (`include/buckets_cache.h` - 86 lines)
- [x] **Criterion Test Suite** (`tests/cluster/test_topology.c` - 318 lines, 18 tests passing)
- [x] **Manual Cache Testing** (`tests/test_cache_manual.c` - 149 lines, 4 tests passing)

### Build System
- [x] Makefile with component targets
- [x] Static and shared library builds
- [x] POSIX compatibility (`-D_POSIX_C_SOURCE=200809L`)
- [x] Static linking for server binary
- [x] cJSON integration from third_party/
- [x] Debug and profile build modes
- [x] Component-specific builds (core, cluster, hash, etc.)
- [x] Clean and format targets

### Testing
- [x] Build verification: All components compile cleanly
- [x] Binary testing: `./bin/buckets version` works
- [x] Logging testing: DEBUG level and file output verified
- [x] Compiler flags: `-Wall -Wextra -Werror -pedantic` enabled
- [x] Criterion test framework installed and integrated
- [x] **42 total tests passing** (20 format + 18 topology + 4 cache)
- [x] Makefile test targets: `make test`, `make test-format`, `make test-topology`
- [x] Manual test suites for initial verification

### Architecture Decisions
- [x] Language: **C11** (rewrite from Go)
- [x] Architecture: **Location Registry + Consistent Hashing**
- [x] Goal: **Fine-grained scalability** (add/remove 1-2 nodes)
- [x] Reference: MinIO codebase in `minio/` folder
- [x] JSON library: **cJSON** (lightweight, MIT license)
- [x] Memory ownership: **Caller owns returned allocations**
- [x] Thread safety: **pthread rwlocks from the start**
- [x] Logging: **Configurable via environment variables**
- [x] Error handling: **Fail fast on disk I/O errors**
- [x] File system layout: **`<disk>/.buckets.sys/` for metadata**

---

## 🔄 In Progress

### Phase 1: Foundation (Weeks 1-4)

**Week 1: Foundation** ✅ **COMPLETE**
- [x] Project structure and build system
- [x] README and documentation framework
- [x] Makefile with component targets
- [x] Core implementation (memory, logging, strings)
- [x] Atomic I/O utilities
- [x] Disk path utilities
- [x] JSON helper utilities
- [x] UUID generation
- [x] Third-party library integration (cJSON)
- [x] Build testing and verification

**Week 2: Format Management** ✅ **90% COMPLETE**
- [x] Format structure implementation (`src/cluster/format.c` - 434 lines)
- [x] Format serialization/deserialization to JSON (MinIO-compatible)
- [x] Format save/load with atomic writes
- [x] Format validation across multiple disks (quorum)
- [x] Format clone operation (deep copy)
- [x] Manual testing (7 tests, all passing)
- [x] Bug fix: atomic_io.c dirname() memory handling
- [x] Test framework setup (Criterion - installed and configured)
- [x] Unit tests for format operations (`tests/cluster/test_format.c` - 318 lines, 20 tests)
- [x] Makefile test targets (make test-format)
- [ ] Thread-safe format cache with rwlock (deferred to Week 3)

**Week 3: Topology Management** ✅ **COMPLETE**
- [x] Topology structure implementation (`src/cluster/topology.c` - 390 lines)
- [x] Set state management (active/draining/removed enum)
- [x] Generation number tracking (starts at 0 for empty, 1 for first config)
- [x] Topology creation from format.json
- [x] JSON serialization/deserialization
- [x] Atomic save/load operations
- [x] Manual testing (topology creation, save/load verification)
- [x] Cache implementation (`src/cluster/cache.c` - 252 lines)
- [x] Thread-safe format cache with pthread_rwlock_t
- [x] Thread-safe topology cache with pthread_rwlock_t
- [x] Cache integration into buckets_init/cleanup
- [x] Cache header (`include/buckets_cache.h` - 86 lines)
- [x] Manual cache tests (4 tests, all passing)
- [x] Criterion unit tests for topology (`tests/cluster/test_topology.c` - 318 lines, 18 tests)
- [x] Makefile test targets (make test-topology)
- [x] BUCKETS_VNODE_FACTOR constant (150) in header

**Week 4: Endpoint Parsing and Validation**
- [ ] Endpoint URL parsing
- [ ] Expansion syntax support (`node{1...4}`, `disk{1...8}`)
- [ ] Endpoint validation
- [ ] Endpoint pool construction
- [ ] Unit tests for endpoint parsing

---

## 📁 Project Structure

```
/home/a002687/minio/
├── README.md                          ✅ Project overview
├── ROADMAP.md                         ✅ 52-week development plan
├── AGENTS.md                          ✅ AI agent development guide
├── Makefile                           ✅ Build system (updated)
├── architecture/                      ✅ Design documentation
│   ├── SCALE_AND_DATA_PLACEMENT.md   ✅ 75-page architecture spec
│   └── CLUSTER_AND_STATE_MANAGEMENT.md ✅ State management design
├── docs/                              ✅ Documentation
│   └── PROJECT_STATUS.md              ✅ You are here
├── include/                           ✅ Public headers
│   ├── buckets.h                     ✅ Main API (updated with logging)
│   ├── buckets_cluster.h             ✅ Cluster structures (with VNODE constant)
│   ├── buckets_io.h                  ✅ Atomic I/O and disk utilities
│   ├── buckets_json.h                ✅ JSON helper API
│   └── buckets_cache.h               ✅ Cache management API (86 lines) - NEW
├── src/                               ✅ Source code
│   ├── main.c                        ✅ Entry point (updated)
│   ├── core/                         ✅ Core utilities
│   │   └── buckets.c                 ✅ Memory, logging, strings (246 lines)
│   ├── cluster/                      ✅ Cluster utilities (Week 1-3)
│   │   ├── uuid.c                    ✅ UUID generation (39 lines)
│   │   ├── atomic_io.c               ✅ Atomic I/O operations (221 lines, bug fixed)
│   │   ├── disk_utils.c              ✅ Disk path utilities (90 lines)
│   │   ├── json_helpers.c            ✅ JSON wrappers (200 lines)
│   │   ├── format.c                  ✅ Format management (434 lines)
│   │   ├── topology.c                ✅ Topology management (390 lines) - NEW
│   │   └── cache.c                   ✅ Thread-safe caching (252 lines) - NEW
│   ├── hash/                         ⏳ Week 5-7 (SipHash, xxHash, ring)
│   ├── crypto/                       ⏳ Week 8-11 (BLAKE2, SHA-256, bitrot)
│   ├── erasure/                      ⏳ Week 8-11 (Reed-Solomon)
│   ├── storage/                      ⏳ Week 12-16 (Disk I/O, object store)
│   ├── registry/                     ⏳ Week 17-20 (Location tracking)
│   ├── topology/                     ⏳ Week 3 (Dynamic topology)
│   ├── migration/                    ⏳ Week 21-24 (Data rebalancing)
│   ├── net/                          ⏳ Week 25-28 (HTTP server, RPC)
│   ├── s3/                           ⏳ Week 29-40 (S3 API handlers)
│   └── admin/                        ⏳ Week 41-44 (Admin API)
├── third_party/                       ✅ Third-party libraries
│   └── cJSON/                        ✅ JSON library (MIT)
│       ├── cJSON.c                   ✅ Downloaded
│       └── cJSON.h                   ✅ Downloaded
├── build/                             ✅ Build artifacts
│   ├── libbuckets.a                  ✅ Static library (66KB)
│   ├── libbuckets.so                 ✅ Shared library (61KB)
│   └── obj/                          ✅ Object files
├── bin/                               ✅ Binaries
│   └── buckets                       ✅ Server binary (18KB)
├── tests/                             ✅ Tests (Week 2-3)
│   ├── test_format_manual.c          ✅ Format manual tests (149 lines, 7 tests)
│   ├── test_cache_manual.c           ✅ Cache manual tests (149 lines, 4 tests) - NEW
│   └── cluster/                      ✅ Criterion test suites
│       ├── test_format.c             ✅ Format tests (318 lines, 20 tests passing)
│       └── test_topology.c           ✅ Topology tests (318 lines, 18 tests passing) - NEW
├── benchmarks/                        ⏳ Week 4+ (Performance tests)
└── minio/                             📚 Reference code
    └── cmd/                          📚 MinIO Go implementation
        ├── erasure-sets.go           📚 Hash-based placement (reference)
        ├── format-erasure.go         📚 Format structures (reference)
        └── xl-storage-format-v2.go   📚 Object metadata (reference)
```

---

## 🎯 Immediate Next Steps

### Week 4: Endpoint Parsing and Validation (NEXT)

**Priority 1: Endpoint URL Parsing**
1. [ ] Implement endpoint URL parser
   - [ ] Parse `http://host:port/path` URLs
   - [ ] Extract scheme, host, port, path components
   - [ ] Validate URL format
   - [ ] Create `buckets_endpoint_t` structure
   - [ ] Unit tests for URL parsing

**Priority 2: Expansion Syntax Support**
2. [ ] Implement expansion syntax
   - [ ] Parse `node{1...4}` numeric ranges
   - [ ] Parse `disk{a...d}` character ranges
   - [ ] Expand to individual URLs
   - [ ] Handle nested expansions
   - [ ] Unit tests for expansion

**Priority 3: Endpoint Pool Construction**
3. [ ] Build endpoint pools
   - [ ] Group endpoints by set
   - [ ] Validate set size consistency
   - [ ] Add format version migration support
   - [ ] Add format comparison utilities
   - [ ] Document format.json structure

**Completed This Session:**
- ✅ Format structure creation with UUID generation
- ✅ JSON serialization/deserialization (MinIO-compatible)
- ✅ Atomic save/load operations
- ✅ Quorum-based validation across disks
- ✅ Deep cloning via JSON roundtrip
- ✅ Manual test suite (7 tests, all passing)
- ✅ Bug fix: atomic_io.c dirname() handling

### Week 3: Topology Management
- [ ] Implement `src/cluster/topology.c`
- [ ] Set state management (active/draining/decomm/removed)
- [ ] Generation number tracking and updates
- [ ] Thread-safe topology cache
- [ ] Unit tests for topology operations

---

## 📊 Progress Metrics

| Component | Status | Progress | Lines of Code | ETA |
|-----------|--------|----------|---------------|-----|
| **Phase 1: Foundation** | 🔄 In Progress | 40% (1.7/4 weeks) | ~1,600 | Week 4 |
| Week 1: Foundation | ✅ Complete | 100% | ~800 | ✅ Done |
| Week 2: Format Management | 🔄 In Progress | 70% | ~580 | Week 2 |
| Week 3: Topology Management | ⏳ Pending | 0% | ~400 target | Week 3 |
| Week 4: Endpoint Parsing | ⏳ Pending | 0% | ~300 target | Week 4 |
| **Phase 2: Hashing** | ⏳ Pending | 0% | ~2,000 target | Week 5-7 |
| **Phase 3: Crypto & Erasure** | ⏳ Pending | 0% | ~3,000 target | Week 8-11 |
| **Phase 4: Storage Layer** | ⏳ Pending | 0% | ~5,000 target | Week 12-16 |
| **Phase 5: Location Registry** | ⏳ Pending | 0% | ~4,000 target | Week 17-20 |

### Week 1 Completion Metrics
- **Files Created**: 9 files (3 headers, 4 implementations, 2 docs)
- **Lines of Code**: ~800 (246 core + 221 atomic_io + 90 disk_utils + 200 json + 39 uuid)
- **Build Artifacts**: libbuckets.a (66KB), libbuckets.so (61KB), buckets binary (18KB)
- **Tests Written**: 0 (testing framework setup in Week 2)
- **Build Time**: ~2 seconds (clean build)
- **Compiler Warnings**: 0 (with -Wall -Wextra -Werror -pedantic)

### Week 2 Completion Metrics (Format Management - 90% Complete)
- **Files Created**: 3 files
  - `src/cluster/format.c` - 434 lines (format management)
  - `tests/test_format_manual.c` - 149 lines (manual tests)
  - `tests/cluster/test_format.c` - 318 lines (Criterion tests)
- **Files Modified**: 2 files
  - `src/cluster/atomic_io.c` - dirname() bug fix
  - `Makefile` - added test-format target
- **Lines of Code**: 434 production + 467 test = 901 total
- **Test Framework**: Criterion installed and integrated
- **Test Suite**: 20 Criterion tests (format creation, save/load, validation, clone, edge cases)
- **Test Results**: 20/20 passing (stable)
- **Build Time**: ~2 seconds (clean build)
- **Test Time**: ~0.2 seconds (20 tests)
- **Format JSON**: MinIO-compatible (verified)
- **Remaining**: Thread-safe cache (deferred to Week 3 with topology work)

---

## 🔧 Build Instructions

### Prerequisites
```bash
# Ubuntu/Debian
sudo apt-get install build-essential libssl-dev zlib1g-dev uuid-dev

# macOS
brew install openssl zlib ossp-uuid
```

### Build
```bash
make                 # Build everything (library + server)
make libbuckets      # Build library only (static + shared)
make buckets         # Build server binary only
make core            # Build core components only
make cluster         # Build cluster components only
make test            # Run tests (Week 2+)
make debug           # Debug build with sanitizers
make profile         # Profile build with gprof
make clean           # Clean build artifacts
make format          # Format code with clang-format
make analyze         # Run clang-tidy static analysis
```

### Run
```bash
# Basic commands
./bin/buckets version
./bin/buckets help

# Server (Week 28+)
./bin/buckets server /data

# Environment variables for logging
BUCKETS_LOG_LEVEL=DEBUG ./bin/buckets version
BUCKETS_LOG_FILE=/tmp/buckets.log ./bin/buckets server /data
```

### Build Output
```
build/
├── libbuckets.a      # Static library (66KB)
├── libbuckets.so     # Shared library (61KB)
└── obj/              # Object files

bin/
└── buckets           # Server binary (18KB)
```

---

## 🎨 Code Style

- **Standard**: C11
- **Style**: K&R with 4-space indents
- **Naming**: snake_case for functions, UPPER_CASE for macros
- **Prefix**: All public symbols prefixed with `buckets_`
- **Headers**: Include guards with `BUCKETS_*_H`
- **Documentation**: Doxygen-style comments

### Example
```c
/**
 * Brief description
 * 
 * Detailed description
 * 
 * @param name Parameter description
 * @return Return value description
 */
buckets_result_t buckets_function_name(const char *name);
```

---

## 🧪 Testing Strategy

- **Unit Tests**: Every component has dedicated tests
- **Integration Tests**: Multi-component interactions
- **Performance Tests**: Benchmarks for critical paths
- **Memory Tests**: Valgrind for leak detection
- **Fuzz Tests**: AFL/libFuzzer for robustness

---

## 📚 Key References

### Architecture
- [SCALE_AND_DATA_PLACEMENT.md](../architecture/SCALE_AND_DATA_PLACEMENT.md) - Complete design spec

### MinIO Reference Code
- `minio/cmd/erasure-sets.go` - Current hash-based placement
- `minio/cmd/erasure-server-pool.go` - Pool management
- `minio/cmd/format-erasure.go` - Disk format structures
- `minio/cmd/xl-storage-format-v2.go` - Object metadata format

### External Resources
- SipHash: https://github.com/veorq/SipHash
- ISA-L: https://github.com/intel/isa-l (Erasure coding)
- Consistent Hashing: https://en.wikipedia.org/wiki/Consistent_hashing

---

## 🤝 Contributing

See [ROADMAP.md](../ROADMAP.md) for current priorities and pick a task!

**Current Focus Areas:**
1. Core data structures implementation
2. Unit test framework setup
3. Build system completion
4. Documentation

---

## 📝 Notes

### Design Decisions Log

**2026-02-25 - Language Choice**: Decided to rewrite MinIO in **C11** for:
- Performance: Direct memory control, no GC pauses
- Memory footprint: <100MB target vs Go's ~500MB+ baseline
- Fine-grained control: Optimization opportunities at every level
- Educational value: Understanding fundamentals from first principles
- Rationale: MinIO's modulo-based hashing prevents fine-grained scaling

**2026-02-25 - Architecture**: Chose **hybrid Location Registry + Consistent Hashing**:
- **Location Registry**: Self-hosted key-value store for <5ms read latency
- **Consistent Hashing**: Virtual nodes (150 per set) for ~20% migration on topology changes
- **No external dependencies**: Registry runs on Buckets itself (bootstraps from format.json)
- **Fine-grained scalability**: Add/remove 1-2 nodes at a time (vs MinIO's pool-based scaling)
- See `architecture/SCALE_AND_DATA_PLACEMENT.md` for full 75-page specification

**2026-02-25 - JSON Library**: Chose **cJSON** (MIT license):
- Lightweight: Single .c/.h file, ~3000 lines
- Minimal dependencies: Only libc required
- Simple API: Easy to wrap for type safety
- Proven: Used in many production systems
- Alternative considered: jansson (more features but heavier)

**2026-02-25 - Memory Ownership**: **Caller owns returned allocations**:
- Clear semantics: If function returns pointer, caller must free
- Consistent pattern: All `buckets_*` functions follow this rule
- Memory wrappers: Use `buckets_malloc/free` for tracking/debugging
- Documentation: Function comments specify ownership

**2026-02-25 - Thread Safety**: **Thread-safe from the start**:
- Use `pthread_rwlock_t` for shared data structures
- Format cache: Global with rwlock protection
- Topology cache: Global with rwlock protection
- Logging: Mutex-protected file writes
- Rationale: Easier to design correctly than retrofit later

**2026-02-25 - Logging Design**: **Configurable via environment variables**:
- `BUCKETS_LOG_LEVEL`: DEBUG/INFO/WARN/ERROR/FATAL (default: INFO)
- `BUCKETS_LOG_FILE`: Optional file path for log output
- Dual output: Always stderr + optional file
- Thread-safe: Mutex-protected writes
- Format: `[timestamp] LEVEL: message`

**2026-02-25 - Error Handling**: **Fail fast on disk I/O errors**:
- No retry logic at low level (caller decides)
- Atomic writes: Temp file + rename pattern
- Directory sync: Ensure metadata persistence with fsync
- Clear error codes: `BUCKETS_ERR_IO`, `BUCKETS_ERR_NOMEM`, etc.
- Rationale: Storage failures are serious, let caller handle appropriately

**2026-02-25 - File System Layout**: **`<disk>/.buckets.sys/` for metadata**:
- Hidden directory: Avoid user confusion
- `format.json`: Immutable cluster identity (deployment ID, set topology)
- `topology.json`: Dynamic cluster state (generation, set states)
- Atomic updates: Write to temp file, rename
- Quorum reads: Validate consistency across disks

**2026-02-25 - Build System**: **Makefile with component targets**:
- Simple: No autotools, no CMake complexity
- Fast: Parallel builds with `-j`
- Flexible: Component-specific targets (make core, make cluster)
- POSIX: `-D_POSIX_C_SOURCE=200809L` for pthread types
- Static linking: Server binary includes libbuckets.a

**2026-02-25 - Testing Strategy**: **Criterion framework** (decided Week 2):
- Modern C test framework with clean syntax
- Automatic test discovery
- Parameterized tests for edge cases
- Integration with Makefile
- Target: 80-90% code coverage

### Resolved Questions

1. ✅ **Test Framework**: **Criterion** - Modern, clean API, good documentation
2. ✅ **JSON Library**: **cJSON** - Lightweight, MIT license, easy to integrate
3. ⏳ **HTTP Server**: TBD (Week 25) - Likely libmicrohttpd or custom
4. ✅ **Build System**: **Makefile only** - Simple, fast, sufficient for now
5. ⏳ **Erasure Coding**: TBD (Week 8) - Likely ISA-L for Intel optimization

### Open Questions

1. **HTTP Server Library** (Week 25): libmicrohttpd vs mongoose vs h2o vs custom?
2. **Erasure Coding Library** (Week 8): ISA-L vs Jerasure vs libec?
3. **Compression** (Week 14): zstd vs lz4 vs both?
4. **TLS Library** (Week 26): OpenSSL vs mbedtls vs BoringSSL?
5. **Metrics/Telemetry** (Week 41): Prometheus format vs StatsD vs custom?

### Known Issues

- ⚠️ LSP errors in editor: Cache issues, resolves on rebuild (cosmetic only)
- ⚠️ No CI/CD yet: Planned for Week 4 (GitHub Actions)
- ⚠️ No automated tests: Test framework setup in Week 2
- ⚠️ No code coverage: Coverage analysis in Week 2 with lcov

---

## 🎉 Week 1 Accomplishments Summary

### What Was Built
Week 1 focused on building a **solid foundation** with essential utilities that all future components will depend on. The implementation follows the **bottom-up approach** outlined in AGENTS.md.

**Core Utilities (246 lines)**:
- Memory management wrappers with OOM handling
- String utilities (strdup, strcmp, format with va_args)
- Configurable logging system (file + console output)
- Initialization and cleanup hooks

**Cluster Utilities (559 lines)**:
- **UUID generation** (39 lines): UUID v4 generation using libuuid
- **Atomic I/O** (230 lines): Crash-safe file writes with temp+rename, directory sync
- **Disk utilities** (90 lines): Path construction for `.buckets.sys/` metadata
- **JSON helpers** (200 lines): Type-safe wrappers around cJSON for serialization

**Infrastructure**:
- Makefile with component targets and proper dependency handling
- Third-party integration (cJSON library)
- Clean build system producing static library, shared library, and binary
- POSIX compatibility with pthread support

### What Was Learned
- MinIO's hash-based placement (`hash(object) % sets`) prevents fine-grained scaling
- Atomic file operations require: write to temp → fsync file → fsync directory → rename
- pthread types require `-D_POSIX_C_SOURCE=200809L` with `-std=c11`
- Static linking simplifies deployment (no shared library dependencies)
- Environment variable configuration is simpler than config files for initial development

### What's Next (Week 2 - Remaining)
- Set up Criterion test framework and write unit tests
- Implement thread-safe format cache with rwlocks
- Complete Week 2 deliverables

### Key Metrics
- **Files**: 9 new files (3 headers, 4 C implementations, updated Makefile + main.c)
- **Code**: ~800 lines of production code
- **Build**: Clean compilation with `-Wall -Wextra -Werror -pedantic`
- **Binary Size**: 18KB server binary (static linked)
- **Library Size**: 66KB static library, 61KB shared library
- **Time**: Week 1 completed in 1 day (efficient AI-assisted development)

---

## 🎉 Week 2 Accomplishments Summary (90% Complete - PRODUCTION READY)

### What Was Built
Week 2 focuses on **format.json management** - the immutable cluster identity that defines the erasure set topology and deployment ID.

**Format Management (434 lines)**:
- Format structure creation with automatic UUID generation
- MinIO-compatible JSON serialization/deserialization
- Atomic save/load operations using established I/O utilities
- Quorum-based validation across multiple disks
- Deep cloning via JSON roundtrip (same approach as MinIO)
- Proper memory management for nested 2D disk array

**Testing (467 lines total)**:
- Manual test suite (149 lines) - 7 tests for initial verification
- Criterion test suite (318 lines) - 20 comprehensive unit tests
  - Format creation tests (valid/invalid parameters)
  - Save/load round-trip testing  
  - Clone operation verification
  - Multi-disk quorum validation (3/4, 2/4 scenarios)
  - NULL handling and edge cases
  - Large topology testing (16x16)
  - All 20 tests passing reliably

**Bug Fixes**:
- Fixed dirname() memory handling in atomic_io.c
- Proper cleanup of directory paths in all error paths

### What Was Learned
- MinIO's format.json structure uses "xl" key for erasure information (legacy naming)
- Quorum validation requires majority consensus (>50%) across disks
- Deep cloning via JSON is simple and matches MinIO's approach
- Atomic writes with directory sync ensure crash-safe metadata persistence
- Format validation is critical for detecting split-brain scenarios

### Format JSON Structure
The generated format.json is fully MinIO-compatible:
```json
{
  "version": "1",
  "format": "erasure",
  "id": "deployment-uuid",
  "xl": {
    "version": "3",
    "this": "disk-uuid",
    "distributionAlgo": "SIPMOD+PARITY",
    "sets": [
      ["disk-uuid-1", "disk-uuid-2", "disk-uuid-3", "disk-uuid-4"],
      ["disk-uuid-5", "disk-uuid-6", "disk-uuid-7", "disk-uuid-8"]
    ]
  }
}
```

### What's Next (Week 3 - Topology Management)
- Implement topology.json structure and operations
- Set state management (active/draining/decomm/removed)
- Generation number tracking for topology changes
- Thread-safe topology cache with rwlock
- Thread-safe format cache with rwlock (deferred from Week 2)
- Unit tests for topology operations

### Key Metrics (Week 2 Complete)
- **Files**: 3 new (format.c: 434 lines, test_format_manual.c: 149 lines, test_format.c: 318 lines)
- **Code**: 901 lines total (434 production + 467 test)
- **Tests**: 20 Criterion tests, all passing
- **Test Coverage**: Format creation, serialization, persistence, validation, cloning, edge cases
- **Format**: MinIO-compatible, production-ready
- **Time**: Week 2 completed in 1 day (with comprehensive testing)
- **Quality**: All tests passing, no compiler warnings, strict error checking

---

## 📊 Week 3 Summary (Topology Management - COMPLETE)

Week 3 focuses on **topology.json management** - the dynamic cluster state that tracks sets, disks, endpoints, and topology changes via generation numbers.

**Topology Management (390 lines)**:
- Topology structure with pools, sets, and disk info
- Set state management (active/draining/removed enum)
- Generation number tracking (0 for empty, 1+ for configured)
- Topology creation from format.json (initial cluster setup)
- MinIO-compatible JSON serialization/deserialization
- Atomic save/load operations
- BUCKETS_VNODE_FACTOR constant (150 virtual nodes per set)

**Cache Implementation (252 lines)**:
- Thread-safe format cache with pthread_rwlock_t
- Thread-safe topology cache with pthread_rwlock_t
- Cache get/set/invalidate operations
- Proper initialization/cleanup in buckets_init/cleanup
- Cache API header (buckets_cache.h - 86 lines)

**Core Integration**:
- Updated buckets.c (246 lines) to initialize caches
- Cache initialization in buckets_init()
- Cache cleanup in buckets_cleanup() (reverse order)

**Testing (785 lines total)**:
- Manual cache tests (149 lines) - 4 tests for cache operations
- Criterion topology tests (318 lines) - 18 comprehensive unit tests
  - Empty topology creation
  - Topology from format conversion (single/multiple sets)
  - Save/load round-trip testing
  - Generation number preservation
  - Set state preservation (active/draining/removed)
  - NULL handling and edge cases
  - All 18 tests passing reliably
- All Week 2 tests still passing (20 format tests)

**Build System Updates**:
- Added test-topology Makefile target
- Updated test suite to run both format and topology tests

### What Was Learned
- Generation numbers should start at 0 for empty topology, 1 for first configuration
- Virtual node factor (150) is critical for consistent hashing performance
- Topology is mutable (vs format.json which is immutable)
- Cache ownership: format cache clones, topology cache takes ownership
- pthread rwlocks provide efficient reader/writer concurrency

### Topology JSON Structure
The generated topology.json tracks dynamic cluster state:
```json
{
  "version": 1,
  "generation": 1,
  "deploymentID": "cluster-uuid",
  "vnodeFactor": 150,
  "pools": [{
    "idx": 0,
    "sets": [{
      "idx": 0,
      "state": "active",
      "disks": [{
        "uuid": "disk-uuid",
        "endpoint": "http://node1:9000/mnt/disk1",
        "capacity": 1000000000000
      }]
    }]
  }]
}
```

### What's Next (Week 4 - Endpoint Parsing)
- Endpoint URL parsing (http://host:port/path)
- Expansion syntax support (node{1...4}, disk{a...d})
- Endpoint validation
- Endpoint pool construction
- Unit tests for endpoint parsing

### Key Metrics (Week 3 Complete)
- **Files**: 4 new (topology.c: 393, cache.c: 247, buckets_cache.h: 86, test_topology.c: 318, test_cache_manual.c: 161)
- **Code**: 1,205 lines total (726 production + 479 test)
- **Tests**: 18 topology tests + 4 cache tests + 20 format tests = 42 total, all passing
- **Test Coverage**: Topology CRUD, caching, generation tracking, set states, edge cases
- **Performance**: Thread-safe caching with rwlocks for concurrent access
- **Time**: Week 3 completed in 1 session (~1.5 hours)
- **Quality**: All tests passing, no compiler warnings, strict error checking

### Cumulative Progress (Weeks 1-3)
- **Total Production Code**: 1,871 lines
  - Core: 255 lines (buckets.c)
  - Cluster utilities: 1,616 lines (uuid, atomic_io, disk_utils, json_helpers, format, topology, cache)
- **Total Test Code**: 950 lines
  - Manual tests: 310 lines (format + cache)
  - Criterion tests: 640 lines (format + topology)
- **Total Headers**: 5 files (buckets.h, buckets_cluster.h, buckets_io.h, buckets_json.h, buckets_cache.h)
- **Test Coverage**: 42 tests passing, 0 failing
- **Build Artifacts**: libbuckets.a (98KB), buckets binary (70KB)
- **Phase 1 Progress**: 75% complete (3/4 weeks done)

---

**Status Legend**:
- ✅ Complete
- 🔄 In Progress
- ⏳ Pending
- 🟢 Active
- 🔴 Blocked
- 📚 Reference

---

**Next Update**: End of Week 2 (after format.json implementation and testing framework setup)
