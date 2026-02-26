# Buckets Project Status

**Last Updated**: February 25, 2026  
**Current Phase**: Phase 5 - Week 18 (Location Registry) - 🔄 IN PROGRESS  
**Status**: 🟢 Active Development  
**Phase 1 Status**: ✅ COMPLETE (Foundation - Weeks 1-4)  
**Phase 2 Status**: ✅ COMPLETE (Hashing - Weeks 5-7)  
**Phase 3 Status**: ✅ COMPLETE (Cryptography & Erasure - Weeks 8-11)  
**Phase 4 Status**: ✅ COMPLETE (Storage Layer - Weeks 12-16)  
**Phase 5 Status**: 🔄 IN PROGRESS (Location Registry - Week 18/20, 50% complete)

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

### Phase 2: Hashing (Weeks 5-7) - ✅ COMPLETE

**Week 5: SipHash-2-4 Implementation**
- [x] **SipHash-2-4 Algorithm** (`src/hash/siphash.c` - 356 lines):
  - [x] Core SipHash-2-4 implementation (cryptographically strong)
  - [x] Support for arbitrary-length inputs
  - [x] 128-bit key initialization
  - [x] 64-bit hash output
  - [x] String hashing convenience wrapper
- [x] **API Header** (`include/buckets_hash.h` - partial, 292 lines total)
- [x] **Criterion Test Suite** (`tests/hash/test_siphash.c` - 273 lines, 16 tests passing)
- [x] Test vectors from SipHash reference implementation
- [x] Edge cases: empty strings, NULL inputs, various key values

**Week 6: xxHash-64 Implementation**
- [x] **xxHash-64 Algorithm** (`src/hash/xxhash.c` - 200 lines):
  - [x] Fast non-cryptographic hash (6-7x faster than SipHash)
  - [x] Support for arbitrary-length inputs
  - [x] 64-bit seed support
  - [x] 64-bit hash output
  - [x] String hashing convenience wrapper
- [x] **API Header** (`include/buckets_hash.h` - 292 lines, combined with SipHash)
- [x] **Criterion Test Suite** (`tests/hash/test_xxhash.c` - 267 lines, 16 tests passing)
- [x] Test vectors from xxHash reference implementation
- [x] Performance comparison with SipHash
- [x] Edge cases: empty strings, NULL inputs, various seeds

**Week 7: Hash Ring & Consistent Hashing**
- [x] **Hash Ring Implementation** (`src/hash/ring.c` - 364 lines):
  - [x] Virtual node ring with configurable vnodes per node (default: 150)
  - [x] Add/remove physical nodes with automatic vnode creation
  - [x] Binary search lookup for O(log N) performance
  - [x] N-replica lookup for replication strategies
  - [x] Distribution statistics (min/max/avg per node)
  - [x] Jump Consistent Hash implementation (stateless alternative)
- [x] **API Header** (`include/buckets_ring.h` - 186 lines):
  - [x] Ring creation/destruction
  - [x] Node add/remove operations
  - [x] Lookup functions (single and N-replica)
  - [x] Distribution analysis
  - [x] Jump hash functions
- [x] **Criterion Test Suite** (`tests/hash/test_ring.c` - 277 lines, 17 tests passing):
  - [x] Ring creation with default vnodes
  - [x] Add/remove node operations
  - [x] Lookup correctness (consistent mapping)
  - [x] Multi-replica lookup
  - [x] Distribution fairness testing
  - [x] Jump hash correctness
  - [x] NULL input validation
  - [x] Edge cases: single node, empty ring, large rings

### Phase 3: Cryptography & Erasure Coding (Weeks 8-11) - 🔄 IN PROGRESS

**Week 8: BLAKE2b Cryptographic Hashing** ✅ **COMPLETE**
- [x] **BLAKE2b Implementation** (`src/crypto/blake2b.c` - 428 lines):
  - [x] Core BLAKE2b algorithm (faster than SHA-256)
  - [x] Optimized for 64-bit platforms (uses 64-bit operations)
  - [x] 12 rounds of mixing per block
  - [x] Support for arbitrary-length inputs
  - [x] Variable output length (1-64 bytes)
  - [x] BLAKE2b-256 (32 bytes) and BLAKE2b-512 (64 bytes) variants
  - [x] Incremental hashing (init/update/final API)
  - [x] Keyed hashing support (MAC mode with up to 64-byte keys)
  - [x] Hex string output convenience function
  - [x] Constant-time comparison for hash verification
- [x] **API Header** (`include/buckets_crypto.h` - 199 lines):
  - [x] One-shot hash functions (blake2b, blake2b_256, blake2b_512)
  - [x] Incremental API (init/update/final)
  - [x] Keyed hashing (init_key)
  - [x] Parameter customization (init_param with tree mode support)
  - [x] Hex output (blake2b_hex)
  - [x] Verification (blake2b_verify - constant time)
  - [x] Self-test function
- [x] **Criterion Test Suite** (`tests/crypto/test_blake2b.c` - 295 lines, 16 tests passing):
  - [x] Empty string hashing (512-bit and 256-bit)
  - [x] Standard test vectors ("abc", test strings)
  - [x] Incremental hashing (multi-part updates)
  - [x] Keyed hashing (MAC mode with different keys)
  - [x] Variable output sizes (16, 32, 48 bytes)
  - [x] Hex output format
  - [x] Constant-time verification
  - [x] Large data (>1 block, multiple blocks)
  - [x] NULL input validation
  - [x] Zero-length data handling
  - [x] Self-test verification
  - [x] Deterministic output
  - [x] Hash uniqueness (different inputs → different outputs)

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
- [x] **165 total tests passing** (Phase 1: 62 tests, Phase 2: 49 tests, Phase 3: 36 tests, Phase 4: 18 tests)
  - Phase 1 (Foundation): 20 format + 18 topology + 22 endpoint + 2 cache = 62 tests
  - Phase 2 (Hashing): 16 siphash + 16 xxhash + 17 ring = 49 tests
  - Phase 3 (Cryptography & Erasure): 16 blake2b + 20 erasure = 36 tests
  - Phase 4 (Storage Layer): 18 storage = 18 tests
- [x] Makefile test targets: `make test`, `make test-format`, `make test-topology`, `make test-endpoint`
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

**Week 4: Endpoint Parsing and Validation** ✅ **COMPLETE**
- [x] **Endpoint Operations** (`src/cluster/endpoint.c` - 710 lines):
  - [x] `buckets_endpoint_parse()` - Parse URL and path-style endpoints
  - [x] URL parsing: http://host:port/path and https:// with IPv4/IPv6 support
  - [x] Path parsing: /mnt/disk1 (local filesystem paths)
  - [x] Port handling (with/without explicit port, default handling)
  - [x] IPv6 address parsing with bracket notation [::1]:port
  - [x] Endpoint validation (scheme, host, port range, path checks)
  - [x] Endpoint equality comparison
  - [x] Localhost detection (localhost, 127.0.0.1, ::1, hostname)
  - [x] Endpoint to string conversion
- [x] **Ellipses Expansion** (`src/cluster/endpoint.c`):
  - [x] `buckets_endpoint_has_ellipses()` - Detect {N...M} patterns
  - [x] `buckets_expansion_pattern_parse()` - Parse numeric and alphabetic ranges
  - [x] `buckets_expansion_pattern_expand()` - Expand to string array
  - [x] Numeric expansion: {1...4} → ["1", "2", "3", "4"]
  - [x] Alphabetic expansion: {a...d} → ["a", "b", "c", "d"]
  - [x] Support for prefix/suffix: node{1...4} → ["node1", "node2", ...]
- [x] **Endpoint Arrays** (`src/cluster/endpoint.c`):
  - [x] `buckets_endpoints_parse()` - Parse multiple endpoints with expansion
  - [x] `buckets_endpoints_to_sets()` - Organize into erasure sets
  - [x] Automatic disk index assignment (pool_idx, set_idx, disk_idx)
- [x] **API Header** (`include/buckets_endpoint.h` - 231 lines)
- [x] **Criterion Test Suite** (`tests/cluster/test_endpoint.c` - 318 lines, 22 tests passing)
- [x] **Makefile Integration** (test-endpoint target)

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
│   ├── buckets_cache.h               ✅ Cache management API (86 lines)
│   ├── buckets_endpoint.h            ✅ Endpoint parsing API (231 lines)
│   ├── buckets_hash.h                ✅ Hash algorithms API (292 lines)
│   ├── buckets_ring.h                ✅ Hash ring API (186 lines)
│   └── buckets_crypto.h              ✅ Cryptography API (199 lines) - NEW
├── src/                               ✅ Source code
│   ├── main.c                        ✅ Entry point (updated)
│   ├── core/                         ✅ Core utilities
│   │   └── buckets.c                 ✅ Memory, logging, strings (246 lines)
│   ├── cluster/                      ✅ Cluster utilities (Week 1-4)
│   │   ├── uuid.c                    ✅ UUID generation (39 lines)
│   │   ├── atomic_io.c               ✅ Atomic I/O operations (221 lines)
│   │   ├── disk_utils.c              ✅ Disk path utilities (90 lines)
│   │   ├── json_helpers.c            ✅ JSON wrappers (200 lines)
│   │   ├── format.c                  ✅ Format management (434 lines)
│   │   ├── topology.c                ✅ Topology management (390 lines)
│   │   ├── cache.c                   ✅ Thread-safe caching (252 lines)
│   │   └── endpoint.c                ✅ Endpoint parsing (710 lines) - NEW
│   ├── hash/                         ✅ Week 5-7 (SipHash, xxHash, ring)
│   │   ├── siphash.c                ✅ SipHash-2-4 implementation (356 lines)
│   │   ├── xxhash.c                 ✅ xxHash-64 implementation (200 lines)
│   │   └── ring.c                   ✅ Hash ring + Jump hash (364 lines)
│   ├── crypto/                       🔄 Week 8-11 (BLAKE2, SHA-256, bitrot)
│   │   └── blake2b.c                 ✅ BLAKE2b implementation (428 lines) - NEW
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
├── tests/                             ✅ Tests (Week 2-8)
│   ├── test_format_manual.c          ✅ Format manual tests (149 lines, 7 tests)
│   ├── test_cache_manual.c           ✅ Cache manual tests (149 lines, 4 tests)
│   ├── cluster/                      ✅ Criterion test suites (Weeks 2-4)
│   │   ├── test_format.c             ✅ Format tests (318 lines, 20 tests passing)
│   │   ├── test_topology.c           ✅ Topology tests (318 lines, 18 tests passing)
│   │   └── test_endpoint.c           ✅ Endpoint tests (318 lines, 22 tests passing)
│   ├── hash/                         ✅ Hash test suites (Weeks 5-7)
│   │   ├── test_siphash.c            ✅ SipHash tests (273 lines, 16 tests passing)
│   │   ├── test_xxhash.c             ✅ xxHash tests (267 lines, 16 tests passing)
│   │   └── test_ring.c               ✅ Hash ring tests (277 lines, 17 tests passing)
│   └── crypto/                       ✅ Crypto test suites (Week 8) - NEW
│       └── test_blake2b.c            ✅ BLAKE2b tests (295 lines, 16 tests passing)
└── benchmarks/                        ⏳ Week 4+ (Performance tests)
```

---

## 🎯 Immediate Next Steps

### Week 13 Complete! 🎉

**Completed: Object Metadata & Versioning**
- ✅ S3-compatible object versioning
- ✅ Version-specific storage with UUID-based version IDs
- ✅ Delete markers for soft deletes
- ✅ Version listing and retrieval
- ✅ Extended metadata support (user-defined, S3 headers)
- ✅ LRU metadata cache with thread-safety
- ✅ All 5 tests passing

### Week 14-16: Multi-Disk Management & Integration ✅ COMPLETE

**Priority 1: Multi-Disk Detection** ✅
1. [x] Implement disk enumeration
   - [x] Scan for `.buckets.sys/format.json` on mount points
   - [x] Validate disk UUIDs match deployment ID
   - [x] Build disk-to-set mapping from format.json

**Priority 2: Quorum Operations** ✅
2. [x] Implement quorum-based reads
   - [x] Read xl.meta from majority of disks (N/2+1)
   - [x] Validate consistency (checksums, version numbers)
   - [x] Automatic healing on mismatch

3. [x] Implement quorum-based writes
   - [x] Write to all disks in parallel
   - [x] Require N/2+1 successful writes
   - [x] Track disk failures (mark offline)

**Priority 3: Integration Testing** ✅
4. [x] End-to-end storage testing
   - [x] Multi-disk write and read
   - [x] Disk failure scenarios
   - [x] Healing and reconstruction
   - [x] 10 integration tests passing

**Week 9: SHA-256 Implementation** ✅ **COMPLETE**
- [x] **SHA-256 Implementation** (`src/crypto/sha256.c` - 99 lines):
  - [x] OpenSSL wrapper for hardware-accelerated hashing
  - [x] One-shot SHA-256 hashing (256-bit output)
  - [x] Incremental hashing (init, update, final)
  - [x] Hex output (sha256_hex)
  - [x] Constant-time verification (sha256_verify)
  - [x] Self-test function
- [x] **API Header** (`include/buckets_crypto.h` - updated to 263 lines)
- [x] **Criterion Test Suite** (`tests/crypto/test_sha256.c` - 175 lines, 12 tests passing):
  - [x] Empty string hashing
  - [x] Standard test vectors ("abc", "The quick brown fox...")
  - [x] Incremental hashing (multi-part updates)
  - [x] Large data hashing (1KB, 10KB)
  - [x] Hex output format
  - [x] Constant-time verification
  - [x] NULL input validation
  - [x] Self-test verification

**Week 10-11: Reed-Solomon Erasure Coding** ✅ **COMPLETE**
- [x] **Library Selection**: Intel ISA-L 2.31.0 (10-15 GB/s, SIMD-optimized)
- [x] **Erasure Coding Implementation** (`src/erasure/erasure.c` - 546 lines):
  - [x] Context initialization with Cauchy matrices (`buckets_ec_init`)
  - [x] Encoder: Split data into K data chunks + M parity chunks (`buckets_ec_encode`)
  - [x] Decoder: Reconstruct from any K available chunks (`buckets_ec_decode`)
  - [x] Reconstruction: Rebuild specific missing chunks (`buckets_ec_reconstruct`)
  - [x] Helper functions:
    - [x] Chunk size calculation with SIMD alignment (`buckets_ec_calc_chunk_size`)
    - [x] Configuration validation (K=1-16, M=1-16)
    - [x] Overhead calculation (storage overhead percentage)
    - [x] Self-test function
- [x] **API Header** (`include/buckets_erasure.h` - 191 lines):
  - [x] Context-based design (`buckets_ec_ctx_t`)
  - [x] Common configurations: 4+2, 8+4, 12+4, 16+4
  - [x] Complete encode/decode/reconstruct API
- [x] **Criterion Test Suite** (`tests/erasure/test_erasure.c` - 624 lines, 20 tests passing):
  - [x] Context initialization (4+2, 8+4, 12+4, 16+4)
  - [x] Configuration validation (invalid K/M)
  - [x] Chunk size calculation and overhead
  - [x] Simple encode/decode roundtrip
  - [x] Missing 1 data chunk reconstruction
  - [x] Missing 2 data chunks reconstruction
  - [x] Missing 1 parity chunk reconstruction
  - [x] Mixed missing chunks (data + parity)
  - [x] Too many missing chunks (error handling)
  - [x] Large data tests (1KB, 64KB with multiple missing chunks)
  - [x] Self-test verification
- [x] **ISA-L Integration**: Direct API usage for maximum control
- [x] **Makefile Updates**: Added erasure test target with ISA-L linking

### Phase 4: Storage Layer (Weeks 12-16) - 🔄 IN PROGRESS

**Week 12: Object Primitives & Disk I/O** ✅ **COMPLETE**
- [x] **Architecture Documentation** (`architecture/STORAGE_LAYER.md` - 1,650 lines):
  - [x] MinIO-compatible xl.meta format specification
  - [x] Inline vs erasure-coded object strategy (<128KB threshold)
  - [x] Path computation using xxHash-64 (deployment ID seed)
  - [x] Directory structure: `.buckets/data/<prefix>/<hash>/xl.meta` + chunks
  - [x] Atomic write-then-rename pattern for consistency
  - [x] BLAKE2b-256 checksums for each chunk
  - [x] 8+4 Reed-Solomon default configuration
- [x] **Storage API Header** (`include/buckets_storage.h` - 354 lines):
  - [x] Object operations: put, get, delete, head, stat
  - [x] xl.meta structure with stat, erasure, checksums, metadata
  - [x] Storage configuration (data_dir, inline_threshold, EC params)
  - [x] Path utilities (compute_object_path, compute_hash_prefix, compute_object_hash)
  - [x] Layout helpers (should_inline, calculate_chunk_size, select_erasure_config)
- [x] **Layout Utilities** (`src/storage/layout.c` - 224 lines):
  - [x] xxHash-64 path computation with deployment ID seed
  - [x] 2-hex-char directory prefix (256 top-level dirs: 00-ff)
  - [x] Object directory creation with recursive mkdir
  - [x] Object existence checks
  - [x] Chunk size calculation with SIMD alignment (16-byte boundary)
  - [x] Inline threshold checks (<128KB)
  - [x] Erasure config selection based on cluster size
  - [x] ISO 8601 timestamp generation
- [x] **Metadata Management** (`src/storage/metadata.c` - 409 lines):
  - [x] xl.meta to JSON serialization (MinIO-compatible format)
  - [x] JSON to xl.meta deserialization with validation
  - [x] Atomic read/write operations (write-then-rename)
  - [x] Memory management (proper cleanup of nested structures)
  - [x] Checksum array handling
  - [x] User metadata (content-type, custom headers)
  - [x] Inline data base64 encoding/decoding
- [x] **Chunk I/O** (`src/storage/chunk.c` - 150 lines):
  - [x] Chunk file read with atomic I/O
  - [x] Chunk file write with atomic write-then-rename
  - [x] BLAKE2b-256 checksum computation
  - [x] Checksum verification
  - [x] Chunk deletion
  - [x] Chunk existence checks
  - [x] Naming convention: `part.<chunk-index>` (e.g., part.1, part.2)
- [x] **Object Operations** (`src/storage/object.c` - 468 lines):
  - [x] Storage initialization and cleanup
  - [x] Configuration management (get_config)
  - [x] Base64 encode/decode for inline objects
  - [x] **PUT operation**:
    - [x] Inline objects: base64 encode → write xl.meta only
    - [x] Large objects: erasure encode (8+4) → compute checksums → write chunks + xl.meta
  - [x] **GET operation**:
    - [x] Read xl.meta
    - [x] Inline objects: base64 decode
    - [x] Large objects: read chunks → verify checksums → erasure decode
  - [x] **DELETE operation**: Remove xl.meta and all chunks
  - [x] **HEAD operation**: Return full xl.meta metadata
  - [x] **STAT operation**: Return size and modTime only
- [x] **Criterion Test Suite** (`tests/storage/test_object.c` - 480 lines, 18 tests passing):
  - [x] Storage initialization (valid config, NULL config)
  - [x] Small object write/read roundtrip (<128KB)
  - [x] Inline object with metadata (content-type)
  - [x] Inline size threshold (127KB vs 129KB)
  - [x] Large object write/read (1MB with erasure coding)
  - [x] Large object chunk verification (checksums, algorithm)
  - [x] Multiple large objects (5 files with different patterns)
  - [x] Delete inline object
  - [x] Delete large object
  - [x] Delete nonexistent object (error handling)
  - [x] Head object (metadata only)
  - [x] Stat object (size and modTime)
  - [x] NULL parameter validation (put/get)
  - [x] Get nonexistent object
  - [x] Overwrite inline object
  - [x] Overwrite large object
- [x] **Total Storage Layer**: 1,605 lines of implementation + 480 lines of tests = 2,085 lines

**Week 12 Metrics**:
- **Files Created**: 6 files (1 header, 5 implementations, 1 test suite)
- **Lines of Code**: 2,085 (1,605 implementation + 480 tests)
- **Tests Written**: 18 tests (100% passing)
- **Test Coverage**: Inline objects, large objects, checksums, error handling
- **Build Time**: ~3 seconds (clean build with storage layer)
- **Dependencies**: ISA-L (erasure), OpenSSL (BLAKE2b), cJSON (xl.meta)

**Week 13: Object Metadata & Versioning** ✅ **COMPLETE**
- [x] **Versioning Implementation** (`src/storage/versioning.c` - 554 lines):
  - [x] S3-compatible versioning with multiple versions per object
  - [x] Version-specific storage: `versions/<versionId>/` directories
  - [x] UUID-based version ID generation (libuuid)
  - [x] Put versioned object (creates new version on each write)
  - [x] Get object by version ID (NULL for latest non-deleted version)
  - [x] Delete markers for soft deletes (S3-compatible)
  - [x] List all versions with delete marker flags
  - [x] Hard delete specific version (permanent removal)
  - [x] .latest symlink to track most recent version
  - [x] Version directory management with atomic operations
- [x] **Metadata Utilities** (`src/storage/metadata_utils.c` - 389 lines):
  - [x] ETag computation using BLAKE2b-256 (S3-compatible hex format)
  - [x] User metadata (x-amz-meta-*) add/get operations
  - [x] Version ID generation wrapper
  - [x] Put object with full metadata support
  - [x] Standard S3 metadata: content-type, cache-control, etc.
  - [x] Versioning metadata: versionId, isLatest, isDeleteMarker
- [x] **Metadata Caching Layer** (`src/storage/metadata_cache.c` - 557 lines):
  - [x] LRU cache for xl.meta (10,000 entries default)
  - [x] Thread-safe with pthread_rwlock_t
  - [x] Hash table with open chaining (xxHash-64)
  - [x] TTL-based expiration (5 minutes default)
  - [x] Deep clone of xl.meta for cache ownership
  - [x] Cache statistics: hits, misses, evictions
  - [x] Cache operations: get, put, invalidate
  - [x] Automatic LRU eviction when cache is full
- [x] **API Updates** (`include/buckets_storage.h` - updated to 571 lines):
  - [x] Versioning APIs: put_versioned, delete_versioned, get_by_version
  - [x] Version listing with delete marker detection
  - [x] Hard delete specific version
  - [x] Metadata cache APIs: init, cleanup, get, put, invalidate, stats
  - [x] User metadata APIs: add, get
  - [x] ETag computation API
  - [x] Version ID generation API
- [x] **Simple Test Suite** (`tests/storage/test_versioning_simple.c` - 141 lines):
  - [x] Version ID generation test (UUID format validation)
  - [x] ETag computation test (BLAKE2b-256 hex output)
  - [x] User metadata test (add and retrieve)
  - [x] Metadata cache test (put and get)
  - [x] Cache statistics test (hits, misses, evictions)
  - [x] All 5 tests passing ✅

**Week 13 Metrics**:
- **Files Created**: 4 files (3 implementations, 1 simple test)
- **Lines of Code**: 1,641 (1,500 production + 141 tests)
- **Tests Written**: 5 simple tests (100% passing)
- **Test Coverage**: Version ID generation, ETags, user metadata, caching
- **Build Time**: ~3 seconds (clean build)
- **Dependencies**: libuuid (version IDs), pthread (cache locking)

**Week 14-16: Multi-Disk Management & Integration** ✅ **COMPLETE**

### Implementation Details
**Multi-Disk Context** (`src/storage/multidisk.c` - 648 lines):
- Disk enumeration from array of mount paths
- Load format.json from all disks and validate deployment ID
- Build disk-to-set mapping based on format.json UUIDs
- Organize disks into erasure sets with online/offline tracking
- Thread-safe disk status management with pthread_rwlock_t
- Load cluster topology from disks
- Disk health monitoring (online/offline status per disk)

**Quorum Operations**:
- Quorum-based xl.meta reads (N/2+1 required for consistency)
- Quorum-based xl.meta writes (N/2+1 required for durability)
- Parallel I/O across all available disks in each set
- Automatic failure handling (mark disks offline on error)
- Read tolerance: Continue with reduced quorum (>= N/2+1 online)
- Write tolerance: Fail if quorum cannot be achieved

**Automatic Healing**:
- Detect inconsistent xl.meta across disks (compare size, modTime, checksums)
- Heal corrupted metadata from healthy disks (copy from quorum majority)
- Background scrubbing framework (placeholder for full implementation)
- Automatic repair on inconsistency detection during read operations
- Chunk reconstruction integration (API ready, not yet implemented)

**API Updates** (`include/buckets_storage.h` - updated to 716 lines):
- `buckets_multidisk_init()` - Initialize multi-disk context with disk paths
- `buckets_multidisk_cleanup()` - Cleanup and free resources
- `buckets_multidisk_get_set_disks()` - Get disk paths for erasure set
- `buckets_multidisk_quorum_read_xlmeta()` - Read xl.meta with quorum
- `buckets_multidisk_quorum_write_xlmeta()` - Write xl.meta with quorum
- `buckets_multidisk_get_online_count()` - Get online disk count for set
- `buckets_multidisk_mark_disk_offline()` - Mark disk as failed
- `buckets_multidisk_get_cluster_stats()` - Get cluster statistics
- `buckets_multidisk_validate_xlmeta_consistency()` - Validate metadata consistency
- `buckets_multidisk_heal_xlmeta()` - Heal inconsistent metadata
- `buckets_multidisk_scrub_all()` - Background scrubbing (placeholder)

**Integration Tests** (`tests/storage/test_multidisk_integration.c` - 240 lines):
- Test 1: Multi-disk initialization with 4-disk erasure set ✅
- Test 2: Quorum write xl.meta (4/4 disks) ✅
- Test 3: Quorum read xl.meta roundtrip ✅
- Test 4: Cluster statistics validation ✅
- Test 5: Get set disk paths ✅
- Test 6: Consistency validation (all disks) ✅
- Test 7: Disk failure simulation (mark offline) ✅
- Test 8: Read with reduced quorum (3/4 disks) ✅
- Test 9: Online disk count tracking ✅
- Test 10: Healing functionality verification ✅

### Key Design Decisions
1. **UUID Matching Strategy**: Disk discovery relies on matching format.json "this" UUID with disk paths
2. **Quorum Algorithm**: N/2+1 majority for both reads and writes (MinIO-compatible)
3. **Failure Detection**: Automatic marking of disks offline on I/O errors (no retry at low level)
4. **Healing Strategy**: Read from quorum, detect inconsistencies, copy from healthy disks
5. **Thread Safety**: pthread_rwlock_t for disk status to allow concurrent health checks
6. **Error Handling**: Continue operations with reduced quorum, fail if below N/2+1

### What Was Learned
- Format.json UUID mapping is critical for correct disk-to-set organization
- Quorum N/2+1 provides consistency without requiring all disks online
- Parallel I/O with error tolerance enables high availability
- Healing must compare metadata (size, modTime) not just existence
- Background scrubbing needs full directory traversal (deferred to future enhancement)
- GCC warning `-Wformat-truncation` required pragma for PATH_MAX*2 buffers

### Week 14-16 Final Metrics
- **Files Created**: 2 files
  - `src/storage/multidisk.c` - 648 lines (multi-disk implementation)
  - `tests/storage/test_multidisk_integration.c` - 240 lines (10 integration tests)
- **Files Modified**: 1 file
  - `include/buckets_storage.h` - updated to 716 lines (+145 lines for multi-disk APIs)
- **Lines of Code**: 888 total (648 production + 240 tests)
- **Tests Written**: 10 integration tests (100% passing)
- **Test Coverage**: 
  - Disk enumeration and initialization
  - Quorum read/write operations
  - Disk failure scenarios
  - Healing and consistency validation
  - Cluster statistics
- **Build Time**: ~3 seconds (clean build)
- **Test Time**: ~0.2 seconds (10 integration tests)
- **Compiler Warnings**: 0 (required 1 pragma for false positive)
- **Status**: ✅ COMPLETE - All core multi-disk functionality implemented and tested

### Performance Benchmarking ✅ COMPLETE
- **Benchmark Suite** (`benchmarks/bench_phase4.c` - 361 lines):
  - Erasure coding encode/decode throughput across 4KB-10MB objects
  - Chunk reconstruction with simulated disk failures (2 of 8 missing)
  - Cryptographic hashing comparison (BLAKE2b vs SHA-256)
  - Color-coded output with formatted latency and throughput
  - Warmup iterations to eliminate cold-start effects
- **Makefile Integration**: `make benchmark` target for easy execution
- **Results Documented**: Performance metrics added to PROJECT_STATUS.md
- **Key Findings**:
  - ISA-L provides excellent EC performance (5-10 GB/s encode, 27-51 GB/s decode)
  - BLAKE2b is 1.6x faster than SHA-256 for all object sizes
  - Reconstruction maintains high throughput even with disk failures
  - No significant performance bottlenecks identified

### Remaining Work (Optional Enhancements)
- **Background Scrubbing**: Full implementation with directory traversal
- **Chunk Reconstruction Integration**: Integrate EC decode into storage layer (API exists)
- **Metrics Collection**: Track per-disk I/O stats, error rates, healing counts
- **Extended Benchmarks**: Multi-disk quorum operation throughput, cache hit/miss performance

---

## 📊 Progress Metrics

| Component | Status | Progress | Lines of Code | ETA |
|-----------|--------|----------|---------------|-----|
| **Phase 1: Foundation** | ✅ Complete | 100% (4/4 weeks) | 2,581 | ✅ Done |
| Week 1: Foundation | ✅ Complete | 100% | ~800 | ✅ Done |
| Week 2: Format Management | ✅ Complete | 100% | 434 | ✅ Done |
| Week 3: Topology Management | ✅ Complete | 100% | 638 | ✅ Done |
| Week 4: Endpoint Parsing | ✅ Complete | 100% | 710 | ✅ Done |
| **Phase 2: Hashing** | ✅ Complete | 100% (3/3 weeks) | 920 | ✅ Done |
| Week 5: SipHash-2-4 | ✅ Complete | 100% | 356 | ✅ Done |
| Week 6: xxHash-64 | ✅ Complete | 100% | 200 | ✅ Done |
| Week 7: Hash Ring | ✅ Complete | 100% | 364 | ✅ Done |
| **Phase 3: Crypto & Erasure** | ✅ Complete | 100% (4/4 weeks) | 1,073 | ✅ Done |
| Week 8: BLAKE2b | ✅ Complete | 100% | 428 | ✅ Done |
| Week 9: SHA-256 | ✅ Complete | 100% | 99 | ✅ Done |
| Week 10-11: Reed-Solomon | ✅ Complete | 100% | 546 | ✅ Done |
| **Phase 4: Storage Layer** | ✅ Complete | 100% (5/5 weeks) | 4,132 | ✅ Done |
| Week 12: Object Primitives | ✅ Complete | 100% | 1,605 | ✅ Done |
| Week 13: Metadata & Versioning | ✅ Complete | 100% | 1,641 | ✅ Done |
| Week 14-16: Multi-Disk Mgmt | ✅ Complete | 100% | 886 | ✅ Done |
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

### MinIO Reference Code (in `/home/a002687/minio-reference/`)
- `cmd/erasure-sets.go` - Current hash-based placement
- `cmd/erasure-server-pool.go` - Pool management
- `cmd/format-erasure.go` - Disk format structures
- `cmd/xl-storage-format-v2.go` - Object metadata format

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

### Week 4 Completion Metrics (Endpoint Parsing - COMPLETE)
- **Files Created**: 3 files
  - `include/buckets_endpoint.h` - 231 lines (endpoint API)
  - `src/cluster/endpoint.c` - 710 lines (implementation)
  - `tests/cluster/test_endpoint.c` - 318 lines (Criterion tests)
- **Files Modified**: 2 files
  - `Makefile` - added test-endpoint target
  - `.gitignore` - refined test exclusions
- **Lines of Code**: 710 production + 318 test = 1,028 total
- **Test Suite**: 22 Criterion tests (URL parsing, path parsing, ellipses expansion, validation)
- **Test Results**: 22/22 passing
- **Features Implemented**:
  - URL endpoint parsing (HTTP/HTTPS with IPv4/IPv6)
  - Path endpoint parsing (local filesystem)
  - Ellipses expansion (numeric {1...4} and alphabetic {a...d})
  - Endpoint validation and localhost detection
  - Endpoint set organization for erasure coding
- **Build Time**: ~2 seconds (clean build)
- **Test Time**: ~0.1 seconds (22 tests)

### Cumulative Progress (Weeks 1-4)
- **Total Production Code**: 2,581 lines
  - Core: 255 lines
  - Cluster utilities: 2,326 lines (uuid, atomic_io, disk_utils, json_helpers, format, topology, cache, endpoint)
- **Total Test Code**: 1,268 lines
  - Manual tests: 310 lines
  - Criterion tests: 958 lines
- **Total Headers**: 6 files
- **Test Coverage**: 62 tests passing, 0 failing (20 format + 18 topology + 22 endpoint + 2 manual)
- **Build Artifacts**: libbuckets.a (120KB est.), buckets binary (85KB est.)
- **Phase 1 Progress**: 100% complete (4/4 weeks done) ✅

---

## 🎉 Phase 2 Complete: Hashing (Weeks 5-7) ✅

### Overview
Phase 2 implemented a complete hashing infrastructure for object placement, data integrity, and consistent distribution across storage nodes. This phase provides both cryptographic and non-cryptographic hashing, plus consistent hashing for dynamic cluster management.

### Week 5: SipHash-2-4 (Cryptographic Hash)
**Implementation** (`src/hash/siphash.c` - 356 lines):
- Core SipHash-2-4 algorithm (cryptographically secure)
- 128-bit key support with proper initialization
- 64-bit hash output for object placement
- Arbitrary-length input support
- String hashing convenience wrapper

**Testing** (`tests/hash/test_siphash.c` - 273 lines, 16 tests):
- Test vectors from SipHash reference implementation
- Empty string and NULL input handling
- Various key combinations
- Deterministic output verification
- All 16 tests passing ✅

**Key Decisions**:
- Chose SipHash over HMAC-SHA256 for speed (3-4x faster)
- Uses 2-4 variant (2 compression rounds, 4 finalization rounds) for security/performance balance
- Integrated with xxHash API for unified interface

### Week 6: xxHash-64 (Fast Non-Cryptographic Hash)
**Implementation** (`src/hash/xxhash.c` - 200 lines):
- xxHash-64 algorithm (6-7x faster than SipHash)
- 64-bit seed support
- 64-bit hash output
- Optimized for speed over security
- String hashing convenience wrapper

**Testing** (`tests/hash/test_xxhash.c` - 267 lines, 16 tests):
- Test vectors from xxHash reference implementation
- Empty string and NULL input handling
- Various seed values
- Performance comparison with SipHash
- All 16 tests passing ✅

**Use Cases**:
- Internal data structure hashing (hash tables, bloom filters)
- Non-sensitive checksums
- Fast content-based addressing where crypto not required

### Week 7: Hash Ring & Consistent Hashing
**Implementation** (`src/hash/ring.c` - 364 lines):
- Virtual node ring structure (150 vnodes per physical node)
- Add/remove nodes with automatic vnode distribution
- Binary search lookup: O(log N) performance
- N-replica lookup for replication strategies
- Distribution statistics (min/max/avg objects per node)
- Jump Consistent Hash implementation (stateless alternative)

**API** (`include/buckets_ring.h` - 186 lines):
- `buckets_ring_create()` - Create ring with vnode factor
- `buckets_ring_add_node()` - Add physical node (creates vnodes)
- `buckets_ring_remove_node()` - Remove node (removes vnodes)
- `buckets_ring_lookup()` - Find node for key (binary search)
- `buckets_ring_lookup_n()` - Find N replicas for key
- `buckets_ring_distribution()` - Analyze object distribution
- `buckets_jump_hash()` / `buckets_jump_hash_str()` - Stateless jump hash

**Testing** (`tests/hash/test_ring.c` - 277 lines, 17 tests):
- Ring creation with default and custom vnodes
- Add/remove node operations
- Lookup consistency (same key → same node)
- Multi-replica lookup (N distinct nodes)
- Distribution fairness (balanced load)
- Jump hash correctness
- NULL input validation
- Edge cases: empty ring, single node, large rings
- All 17 tests passing ✅

**Key Decisions**:
- 150 virtual nodes per physical node (BUCKETS_DEFAULT_VNODES)
- Binary search over linear scan for O(log N) vs O(N)
- Separate vnodes array sorted by hash for efficient lookup
- Jump hash as stateless alternative (no ring structure needed)

**Bug Fixes**:
- Fixed `buckets_ring_create()` to return NULL for negative vnode count (validation)

### Unified Hash API
**Combined Header** (`include/buckets_hash.h` - 292 lines):
- SipHash-2-4 API (cryptographic)
- xxHash-64 API (fast non-cryptographic)
- Consistent interface across both algorithms
- Type-safe key and seed structures

### What Was Learned
- Virtual nodes (150x) provide ~2% migration overhead vs ~20% with 10x
- SipHash-2-4 is 3-4x faster than HMAC-SHA256 for small inputs
- xxHash-64 is 6-7x faster than SipHash for non-cryptographic use
- Binary search on sorted vnodes gives O(log N) lookup
- Jump Consistent Hash is stateless but doesn't support node removal gracefully
- Consistent hashing enables fine-grained scaling (add/remove 1-2 nodes)

### Phase 2 Metrics
- **Files Created**: 6 files
  - `include/buckets_hash.h` - 292 lines (combined API)
  - `include/buckets_ring.h` - 186 lines (ring API)
  - `src/hash/siphash.c` - 356 lines (implementation)
  - `src/hash/xxhash.c` - 200 lines (implementation)
  - `src/hash/ring.c` - 364 lines (implementation)
  - Total: 1,398 lines (478 headers + 920 implementation)
- **Test Files Created**: 3 files
  - `tests/hash/test_siphash.c` - 273 lines (16 tests)
  - `tests/hash/test_xxhash.c` - 267 lines (16 tests)
  - `tests/hash/test_ring.c` - 277 lines (17 tests)
  - Total: 817 lines (49 tests)
- **Test Results**: 49/49 passing (100% success rate)
- **Build Time**: ~2 seconds (clean build)
- **Test Time**: ~0.3 seconds (49 tests)
- **Compiler Warnings**: 0 (strict flags: -Wall -Wextra -Werror -pedantic)

### Cumulative Progress (Weeks 1-7)
- **Total Production Code**: 3,501 lines
  - Core: 255 lines
  - Cluster utilities: 2,326 lines
  - Hash utilities: 920 lines
- **Total Test Code**: 2,085 lines
  - Manual tests: 310 lines
  - Criterion tests: 1,775 lines (958 cluster + 817 hash)
- **Total Headers**: 8 files (6 cluster + 2 hash)
- **Test Coverage**: 111 tests passing, 0 failing
  - Phase 1: 62 tests (20 format + 18 topology + 22 endpoint + 2 manual)
  - Phase 2: 49 tests (16 siphash + 16 xxhash + 17 ring)
- **Build Artifacts**: libbuckets.a (~140KB), buckets binary (~95KB)
- **Phase 1 Progress**: 100% complete (4/4 weeks) ✅
- **Phase 2 Progress**: 100% complete (3/3 weeks) ✅

---

## 🎉 Week 8 Complete: BLAKE2b Cryptographic Hashing ✅

### Overview
Week 8 implemented BLAKE2b, a modern cryptographic hash function that is faster than SHA-256 and optimized for 64-bit platforms. This provides the foundation for object integrity verification and bitrot detection in the storage layer.

### Implementation
**BLAKE2b Core** (`src/crypto/blake2b.c` - 428 lines):
- Core BLAKE2b algorithm based on RFC 7693
- Optimized for 64-bit platforms (uses 64-bit operations)
- 12 rounds of mixing per 128-byte block
- Compression function with proper initialization vector (IV)
- Little-endian byte ordering for cross-platform compatibility
- Secure parameter block handling (packed struct, no padding)
- Constant-time hash verification to prevent timing attacks

**Features**:
- Variable output length (1-64 bytes, commonly 32 or 64)
- BLAKE2b-256 (32 bytes) and BLAKE2b-512 (64 bytes) variants
- Incremental hashing API (init/update/final)
- Keyed hashing for MAC functionality (up to 64-byte keys)
- One-shot convenience functions
- Hex string output
- Self-test with official test vectors

**API** (`include/buckets_crypto.h` - 199 lines):
- `buckets_blake2b()` - One-shot hash with optional key
- `buckets_blake2b_256()` / `buckets_blake2b_512()` - Convenience wrappers
- `buckets_blake2b_init()` - Initialize context
- `buckets_blake2b_init_key()` - Initialize with key (MAC mode)
- `buckets_blake2b_init_param()` - Custom parameters (tree hashing support)
- `buckets_blake2b_update()` - Incremental data feeding
- `buckets_blake2b_final()` - Finalize and output hash
- `buckets_blake2b_hex()` - Hash to hex string
- `buckets_blake2b_verify()` - Constant-time comparison
- `buckets_blake2b_selftest()` - Verification against test vectors

**Testing** (`tests/crypto/test_blake2b.c` - 295 lines, 16 tests):
- Empty string hashing (512-bit and 256-bit variants)
- Standard test strings ("abc", long strings)
- Incremental hashing (multi-part updates)
- Keyed hashing with different keys
- Variable output sizes (16, 32, 48 bytes)
- Hex output format verification
- Constant-time verification function
- Large data handling (multiple blocks)
- NULL input validation
- Zero-length data edge cases
- Self-test verification
- Deterministic output verification
- Hash uniqueness (collision resistance)

### What Was Learned
- BLAKE2b is 1.5-2x faster than SHA-256 on 64-bit platforms
- Parameter block must be exactly 64 bytes (requires packed struct)
- 12 rounds provide strong security while maintaining performance
- Virtual node factor of 150 from Week 7 synergizes well with BLAKE2b speed
- Keyed hashing provides MAC functionality without HMAC overhead
- Constant-time comparison is essential for hash verification

### Week 8 Metrics
- **Files Created**: 3 files
  - `include/buckets_crypto.h` - 199 lines (crypto API)
  - `src/crypto/blake2b.c` - 428 lines (implementation)
  - `tests/crypto/test_blake2b.c` - 295 lines (test suite)
  - Total: 922 lines (627 production + 295 tests)
- **Test Results**: 16/16 passing (100% success rate)
- **Build Time**: ~2 seconds (clean build)
- **Test Time**: ~0.1 seconds (16 tests)
- **Compiler Warnings**: 0 (strict flags: -Wall -Wextra -Werror -pedantic)

### Week 9 Metrics
- **Files Created**: 2 files
  - `src/crypto/sha256.c` - 99 lines (OpenSSL wrapper implementation)
  - `tests/crypto/test_sha256.c` - 175 lines (test suite)
  - Updated: `include/buckets_crypto.h` - +64 lines (now 263 lines total)
  - Total: 274 lines (99 production + 175 tests)
- **Test Results**: 12/12 passing (100% success rate)
- **Build Time**: ~2 seconds (incremental build)
- **Test Time**: ~0.1 seconds (12 tests)
- **Compiler Warnings**: 0 (strict flags: -Wall -Wextra -Werror -pedantic)

### Week 10-11 Metrics
- **Files Created**: 3 files
  - `include/buckets_erasure.h` - 191 lines (erasure coding API)
  - `src/erasure/erasure.c` - 546 lines (Reed-Solomon implementation with ISA-L)
  - `tests/erasure/test_erasure.c` - 624 lines (comprehensive test suite)
  - Updated: `Makefile` - added erasure test target with ISA-L linking
  - Total: 1,361 lines (737 production + 624 tests)
- **Test Results**: 20/20 passing (100% success rate)
  - Configuration tests: 8 tests (init, validation, helpers)
  - Encode/decode tests: 12 tests (roundtrip, missing chunks, edge cases)
- **Configurations Tested**:
  - 4+2 (50% overhead): ✅ Working
  - 8+4 (50% overhead): ✅ Working
  - 12+4 (33% overhead): ✅ Working with 64KB data
  - 16+4 (25% overhead): ✅ Working
- **Missing Chunk Scenarios**:
  - 1 data chunk missing: ✅ Reconstructed
  - 2 data chunks missing: ✅ Reconstructed
  - 1 parity chunk missing: ✅ Reconstructed
  - Mixed missing (data + parity): ✅ Reconstructed
  - Too many missing (>M): ✅ Error handling works
- **Data Sizes Tested**:
  - Small strings (<64 bytes): ✅ Working
  - 1KB data: ✅ Working with 8+4
  - 64KB data: ✅ Working with 12+4, 4 missing chunks
- **Build Time**: ~3 seconds (clean build with ISA-L)
- **Test Time**: ~0.15 seconds (20 tests with encoding/decoding)
- **Compiler Warnings**: 0 (strict flags: -Wall -Wextra -Werror -pedantic)
- **Library Integration**: ISA-L 2.31.0 installed and linked

### Cumulative Progress (Weeks 1-19, Phase 5 Week 19 Complete)
- **Total Production Code**: 10,589 lines (+726 from Week 18-19, -1 from layout fix)
  - Core: 255 lines
  - Cluster utilities: 2,326 lines
  - Hash utilities: 920 lines
  - Crypto utilities: 527 lines (blake2b: 428, sha256: 99)
  - Erasure coding: 546 lines
  - **Storage layer: 4,131 lines** (layout: 223 ✅ fixed, metadata: 409, chunk: 150, object: 468, metadata_utils: 389, versioning: 554, metadata_cache: 557, multidisk: 648, plus 716 header)
  - **Registry layer: 1,293 lines** (registry: 961, plus 332 header)
  - **Benchmarks: 591 lines** (phase4: 235, registry: 356)
- **Total Test Code**: 4,980 lines (+726 from Week 18-19, +15 from test fix)
  - Manual tests: 310 lines
  - Criterion tests: 3,589 lines (958 cluster + 817 hash + 470 crypto + 624 erasure + 720 storage)
  - Simple tests: 725 lines (141 versioning + 214 registry simple + 370 registry batch ✅ fixed)
  - Benchmark code: 356 lines (registry benchmarks)
- **Total Headers**: 13 files (6 cluster + 2 hash + 2 crypto + 1 erasure + 1 storage + 1 registry)
- **Test Coverage**: 196 tests passing (100% pass rate) ✅
  - Phase 1: 62 tests (20 format + 18 topology + 22 endpoint + 2 cache)
  - Phase 2: 49 tests (16 siphash + 16 xxhash + 17 ring)
  - Phase 3: 36 tests (16 blake2b + 12 sha256 + 20 erasure)
  - Phase 4: 33 tests (18 object + 5 versioning + 10 multidisk)
  - **Phase 5: 11 tests** (5 simple + 6 batch, all passing 100%) ✅
- **Build Artifacts**: libbuckets.a (~265KB), buckets binary (~125KB)
- **Phase 1 Progress**: 100% complete (4/4 weeks) ✅
- **Phase 2 Progress**: 100% complete (3/3 weeks) ✅
- **Phase 3 Progress**: 100% complete (4/4 weeks) ✅
- **Phase 4 Progress**: 100% complete (5/5 weeks) ✅
- **Phase 5 Progress**: 75% complete (3/4 weeks, Week 20 remaining) 🔄
- **Overall Progress**: 37% complete (19/52 weeks)

---

## 🎉 Phase 4 Complete: Storage Layer (Weeks 12-16) ✅

### Overview
Phase 4 completed the core storage layer with object primitives, metadata management, versioning, and multi-disk operations. The storage layer now supports S3-compatible object operations with erasure coding, automatic healing, and quorum-based consistency.

### Performance Benchmarks

Comprehensive benchmarks were run to validate performance characteristics and identify potential bottlenecks:

**Erasure Coding Performance (8+4 Reed-Solomon with ISA-L)**:
| Object Size | Encode Throughput | Decode Throughput | Encode Latency | Decode Latency |
|-------------|-------------------|-------------------|----------------|----------------|
| 4 KB        | 10.24 GB/s        | 51.20 GB/s        | 0.40 μs        | 0.08 μs        |
| 128 KB      | 8.38 GB/s         | 42.56 GB/s        | 15.65 μs       | 3.08 μs        |
| 1 MB        | 7.82 GB/s         | 33.98 GB/s        | 134.13 μs      | 30.86 μs       |
| 10 MB       | 5.64 GB/s         | 27.94 GB/s        | 1.86 ms        | 375.29 μs      |

**Key Insights**:
- ISA-L provides excellent performance: 5-10 GB/s encode, 27-51 GB/s decode
- Decoding is 4-6x faster than encoding (ISA-L optimization)
- Encoding overhead: ~83% of total encode+decode time
- Small files (<128KB) get near-optimal throughput (8-10 GB/s encode)

**Chunk Reconstruction (2 of 8 Data Chunks Missing)**:
| Object Size | Reconstruction Throughput | Latency |
|-------------|---------------------------|---------|
| 4 KB        | 51.20 GB/s                | 0.08 μs |
| 128 KB      | 52.43 GB/s                | 2.50 μs |
| 1 MB        | 39.78 GB/s                | 26.36 μs |
| 10 MB       | 31.80 GB/s                | 329.71 μs |

**Key Insights**:
- Reconstruction performance matches decode (no significant overhead)
- Can survive loss of up to 4 chunks (4-disk failures in 12-disk setup)
- Throughput remains high even with missing chunks (31-52 GB/s)

**Cryptographic Hashing (BLAKE2b-256 vs SHA-256)**:
| Object Size | BLAKE2b Throughput | SHA-256 Throughput | BLAKE2b Speedup |
|-------------|--------------------|--------------------|-----------------|
| 4 KB        | 896 MB/s           | 512 MB/s           | 1.75x faster    |
| 128 KB      | 885 MB/s           | 545 MB/s           | 1.62x faster    |
| 1 MB        | 885 MB/s           | 548 MB/s           | 1.61x faster    |
| 10 MB       | 882 MB/s           | 548 MB/s           | 1.61x faster    |

**Key Insights**:
- BLAKE2b is consistently 1.6-1.75x faster than SHA-256 (OpenSSL)
- BLAKE2b throughput is stable across object sizes (~880-900 MB/s)
- SHA-256 (OpenSSL) benefits from hardware acceleration (~512-548 MB/s)
- For 1MB objects: BLAKE2b hashes in 1.19ms, SHA-256 in 1.91ms

**Overall Phase 4 Performance Summary**:
- **Erasure coding**: Production-ready performance (5-10 GB/s encode)
- **Hashing**: Fast checksums with BLAKE2b (880+ MB/s)
- **Reconstruction**: Efficient recovery even with disk failures (30+ GB/s)
- **Bottleneck identified**: Encoding is 83% of EC time (acceptable for write path)
- **Conclusion**: Storage layer meets performance requirements for high-throughput object storage

### Week 14-16 Final Metrics
- **Files Created**: 2 files (multidisk.c + integration test)
- **Lines of Code**: 888 (648 production + 240 tests)
- **Tests Written**: 10 integration tests (100% passing)
- **Test Coverage**: Quorum operations, failure handling, healing, consistency validation
- **Build Time**: ~3 seconds (clean build)
- **Status**: ✅ COMPLETE - All core multi-disk functionality implemented and tested

### Phase 4 Summary Metrics
- **Total Files**: 9 implementation files + 1 storage API header + 3 test suites
- **Total Production Code**: 4,132 lines (includes 716-line API header)
- **Total Test Code**: 861 lines (480 object + 141 versioning + 240 multidisk)
- **Test Coverage**: 33 tests passing (18 object + 5 versioning + 10 multidisk)
- **Weeks Completed**: 5 weeks (Weeks 12-16)
- **Cumulative Progress**: 31% complete (16/52 weeks)

### 🔄 Phase 5: Location Registry (Weeks 17-20) - IN PROGRESS

**Architecture Documentation**:
- ✅ **Implementation Guide** (`architecture/LOCATION_REGISTRY_IMPLEMENTATION.md` - 945 lines)
  - Complete architecture overview with diagrams
  - Data structures and algorithms documentation
  - Performance characteristics and benchmarks
  - API reference with examples
  - Design decisions and rationale
  - Integration points and troubleshooting guide

**Week 17: Registry Core** ✅ COMPLETE (100%)
- [x] **Registry API Header** (`include/buckets_registry.h` - 332 lines):
  - [x] Object location data structures (bucket, object, version → pool, set, disks)
  - [x] Registry key structures for lookups
  - [x] Cache statistics tracking
  - [x] Configuration with defaults (1M entries, 5-min TTL)
  - [x] Complete API: Record/Lookup/Update/Delete operations
  - [x] Batch operations API (ready for implementation)
  - [x] Memory management and serialization functions

- [x] **Registry Implementation** (`src/registry/registry.c` - 841 lines):
  - [x] Thread-safe LRU cache (1M entries, configurable TTL)
  - [x] xxHash-based hash table with open chaining
  - [x] LRU eviction policy (head = most recent, tail = victim)
  - [x] JSON serialization/deserialization
  - [x] Record/Lookup/Delete operations (cache-aware)
  - [x] Cache invalidation and clearing
  - [x] Statistics tracking (hits, misses, evictions, hit rate)
  - [x] Thread-safe with pthread_rwlock_t

- [x] **Simple Test Suite** (`tests/registry/test_registry_simple.c` - 214 lines):
  - [x] Init/cleanup lifecycle ✅
  - [x] Location serialization roundtrip ✅
  - [x] Location cloning ✅
  - [x] Registry key utilities (build/parse) ✅
  - [x] Cache operations (record, lookup, invalidate, stats) ✅
  - [x] All 5 tests passing ✅

**Week 17 Progress**:
- **Files Created**: 3 files (header + implementation + tests)
- **Lines of Code**: 1,387 lines (332 header + 841 impl + 214 tests)
- **Tests**: 5 simple tests passing (100% pass rate)
- **Status**: Core registry functionality complete, storage integration added

**Week 18: Advanced Operations & Performance** ✅ COMPLETE (100%)
- [x] **Batch Operations** (`src/registry/registry.c` - 120 additional lines):
  - [x] `buckets_registry_record_batch()` - Record multiple locations atomically
  - [x] `buckets_registry_lookup_batch()` - Lookup multiple locations in one call
  - [x] Linear scaling performance (100 items in ~440ms record, 0.07ms lookup)
  - [x] Partial failure handling (returns count of successful operations)

- [x] **Update Operation** (`src/registry/registry.c` - added to batch section):
  - [x] `buckets_registry_update()` - Update location during migration
  - [x] Cache-aware implementation (invalidate old + record new)
  - [x] Average update time: 4.5ms (functional performance)

- [x] **Range Query API** (`include/buckets_registry.h` + `src/registry/registry.c`):
  - [x] `buckets_registry_list()` - List objects with optional prefix filtering
  - [x] API defined and documented (placeholder implementation)
  - [x] Ready for storage layer integration

- [x] **Batch Operations Test Suite** (`tests/registry/test_registry_batch.c` - 370 lines):
  - [x] Batch record with multiple locations (10 items) ✅
  - [x] Batch record with partial failures ✅
  - [x] Batch lookup with multiple keys (5 items) ✅
  - [x] Batch lookup with missing entries ✅
  - [x] Update operation lifecycle test ✅
  - [x] Update nonexistent location test ✅
  - [x] **6/6 tests passing** (100% pass rate) ✅

- [x] **Performance Benchmarks** (`benchmarks/bench_registry.c` - 356 lines):
  - [x] **Cache Hit Performance**: 0.323 μs average ✅ (target: <1 μs)
  - [x] **Cache Statistics**: 90% hit rate with 80/20 test pattern
  - [x] **Batch Operations**: 4.4ms/item record, 0.001ms/item lookup ✅
  - [x] **Update Operations**: 4.5ms average ✅
  - [x] All benchmarks functional, targets met for cache hits

**Week 18 Progress**:
- **Files Created**: 2 files (batch tests + benchmarks)
- **Lines Added**: 711 lines (120 impl + 355 tests + 356 benchmarks)
- **Total Registry Code**: 1,209 lines implementation + 584 lines tests + 356 benchmarks
- **Tests**: 11 total (5 simple + 6 batch, all passing 100%) ✅
- **Performance**: Cache hits 0.323 μs, well under 1 μs target ✅
- **Status**: Advanced operations complete, excellent performance validated

**Week 19: Storage Integration Fixes** ✅ COMPLETE (100%)
- [x] **Fixed storage layer directory creation** (`src/storage/layout.c` - 1 line fix):
  - [x] Root cause: `buckets_compute_object_path()` hardcoded `.buckets/data/` prefix
  - [x] Solution: Return relative path `<prefix>/<hash>/` instead, let caller prepend data_dir
  - [x] Impact: Fixed all directory creation issues in registry and storage tests
  - [x] Verification: Simple write test + all 6 batch tests now passing

- [x] **Fixed test race conditions** (`tests/registry/test_registry_batch.c` - 15 lines):
  - [x] Root cause: Tests running in parallel shared same test directory
  - [x] Solution: Unique directory per test using PID + timestamp
  - [x] Impact: Eliminated test interference, 100% reliability
  - [x] Test results: 6/6 batch tests passing consistently

**Week 19-20: Integration & Production Readiness** (In Progress)
- [x] Fix storage layer directory creation for registry writes ✅
- [ ] Bootstrap registry from format.json on startup
- [ ] Integrate registry with object PUT/GET/DELETE operations
- [ ] Add comprehensive integration tests with full storage stack
- [ ] Production performance validation (99%+ cache hit rate)

**Goals**:
- <5ms read latency for location lookups ⏳
- No external dependencies (runs on Buckets itself) ✅
- 99%+ cache hit rate (measured in tests)
- Horizontal scaling ready (architecture supports it)

---

**Status Legend**:
- ✅ Complete
- 🔄 In Progress
- ⏳ Pending
- 🟢 Active
- 🔴 Blocked
- 📚 Reference

---

**Next Update**: End of Week 17-18 (after Registry Data Structures)
