# Buckets Development Roadmap

## Overview

This roadmap tracks the development of Buckets from initial C foundation through full S3-compatible object storage with fine-grained scalability.

**Total Estimated Timeline**: 6-9 months  
**Current Phase**: Foundation

---

## Phase 1: Foundation (Weeks 1-4) ⏳ IN PROGRESS

**Goal**: Establish C project infrastructure and core data structures

### Week 1-2: Project Setup
- [x] Project structure and build system
- [x] README and documentation framework
- [ ] Makefile with component targets
- [ ] CMake configuration
- [ ] Docker build environment
- [ ] CI/CD pipeline (GitHub Actions)
- [ ] Code formatting (clang-format)
- [ ] Static analysis (clang-tidy, cppcheck)

### Week 3-4: Core Data Structures
- [ ] Dynamic arrays (vector)
- [ ] Hash tables
- [ ] Linked lists
- [ ] Ring buffers
- [ ] Memory pools
- [ ] String handling utilities
- [ ] Unit test framework
- [ ] Benchmarking harness

**Deliverables**:
- [ ] `src/core/` - All fundamental data structures
- [ ] `tests/core/` - >90% test coverage
- [ ] Performance benchmarks vs glib

---

## Phase 2: Cryptography & Hashing (Weeks 5-7)

**Goal**: Implement hashing algorithms and cryptographic primitives

### Core Hashing
- [ ] SipHash-2-4 implementation
- [ ] xxHash (for checksums)
- [ ] MD5 (S3 compatibility)
- [ ] SHA-256
- [ ] Hash function benchmarks

### Consistent Hashing
- [ ] Virtual node ring structure
- [ ] Binary search on sorted ring
- [ ] Ring rebuilding on topology changes
- [ ] Migration planning (old vs new ring comparison)
- [ ] Unit tests with various ring sizes

### Cryptography
- [ ] Integrate OpenSSL/LibreSSL
- [ ] TLS support
- [ ] AES encryption
- [ ] RSA key handling
- [ ] Random number generation (CSPRNG)

**Deliverables**:
- [ ] `src/hash/` - All hashing implementations
- [ ] `src/crypto/` - Cryptographic primitives
- [ ] Performance: <20ns for hash ring lookup

---

## Phase 3: Erasure Coding (Weeks 8-11)

**Goal**: Reed-Solomon erasure coding implementation

### Dependencies
- [ ] Evaluate libraries: ISA-L, Jerasure, liberasurecode
- [ ] Choose primary library (ISA-L recommended)
- [ ] Build integration and linking

### Implementation
- [ ] Erasure coding interface
- [ ] Encode operations (data → shards)
- [ ] Decode operations (shards → data)
- [ ] Shard distribution calculation
- [ ] Parity calculation
- [ ] Healing/reconstruction logic

### Testing
- [ ] Unit tests with various N+K configurations
- [ ] Failure scenarios (missing shards)
- [ ] Performance benchmarks
- [ ] Memory leak testing

**Deliverables**:
- [ ] `src/erasure/` - Erasure coding engine
- [ ] Support for 4-16 drive configurations
- [ ] >1 GB/s encode/decode throughput

---

## Phase 4: Storage Layer (Weeks 12-16)

**Goal**: Disk I/O and object storage primitives

### Disk Management
- [ ] Disk detection and enumeration
- [ ] Disk health monitoring
- [ ] Disk UUID assignment
- [ ] Format metadata (format.json equivalent)
- [ ] Quorum-based reads/writes

### Object Storage
- [ ] xl.meta format (C struct + serialization)
- [ ] Object part storage
- [ ] Version handling
- [ ] Directory structure management
- [ ] Atomic write operations

### I/O Layer
- [ ] Direct I/O support
- [ ] Buffered I/O
- [ ] io_uring integration (Linux)
- [ ] AIO fallback (POSIX)
- [ ] I/O scheduling and prioritization

**Deliverables**:
- [ ] `src/storage/` - Storage abstraction layer
- [ ] Direct I/O with O_DIRECT
- [ ] Atomic metadata updates

---

## Phase 5: Location Registry (Weeks 17-20)

**Goal**: Implement self-hosted location registry per architecture spec

### Registry Storage
- [ ] Registry bucket (.buckets-registry)
- [ ] JSON serialization (cJSON or similar)
- [ ] Object location schema
- [ ] Registry key/value interface
- [ ] Quorum writes to registry

### Caching
- [ ] LRU cache implementation (1M entries)
- [ ] Cache invalidation logic
- [ ] TTL management
- [ ] Cache statistics/metrics

### Operations
- [ ] Record location on PUT
- [ ] Lookup location on GET
- [ ] Update location during migration
- [ ] Delete location on object delete
- [ ] Batch operations

**Deliverables**:
- [ ] `src/registry/` - Location registry
- [ ] 99%+ cache hit rate
- [ ] <1ms cache miss latency

---

## Phase 6: Topology Management (Weeks 21-24)

**Goal**: Cluster topology tracking and coordination

### Topology Data Structures
- [ ] ClusterTopology C structs
- [ ] PoolTopology, SetTopology, DiskInfo
- [ ] Set states (Active, Draining, Removed)
- [ ] Generation tracking

### Persistence
- [ ] topology.json serialization
- [ ] Quorum writes to all disks
- [ ] Quorum reads on startup
- [ ] Topology versioning

### Operations
- [ ] Add set to pool
- [ ] Remove set from pool
- [ ] Mark set as draining
- [ ] Topology broadcast to peers

**Deliverables**:
- [ ] `src/topology/` - Topology manager
- [ ] Topology changes in <1 second
- [ ] Automatic peer synchronization

---

## Phase 7: Migration Engine (Weeks 25-30)

**Goal**: Background data migration on topology changes

### Scanner
- [ ] Object enumeration across sets
- [ ] Affected object identification
- [ ] Migration queue generation

### Workers
- [ ] Parallel migration workers (16 threads)
- [ ] Object read from source set
- [ ] Object write to destination set
- [ ] Registry update (atomic)
- [ ] Source cleanup

### Orchestration
- [ ] Migration state machine
- [ ] Progress tracking
- [ ] Pause/resume capability
- [ ] Checkpointing (every 1000 objects)
- [ ] Interruption recovery

### Throttling
- [ ] Bandwidth limiting
- [ ] I/O prioritization (user > migration)
- [ ] CPU throttling

**Deliverables**:
- [ ] `src/migration/` - Migration orchestrator
- [ ] >500 MB/s migration throughput
- [ ] Resumable after crash

---

## Phase 8: Network Layer (Weeks 31-34)

**Goal**: HTTP/S server and peer communication

### HTTP Server
- [ ] Evaluate libraries: libmicrohttpd, mongoose, h2o
- [ ] HTTP/1.1 support
- [ ] HTTP/2 support (optional)
- [ ] TLS integration
- [ ] Request routing
- [ ] Connection pooling

### Peer Communication
- [ ] Grid system (WebSocket-based RPC)
- [ ] Peer discovery
- [ ] Health checking
- [ ] RPC message format
- [ ] Broadcast primitives

**Deliverables**:
- [ ] `src/net/` - Network layer
- [ ] <1ms local RPC latency
- [ ] Connection reuse and pooling

---

## Phase 9: S3 API (Weeks 35-42)

**Goal**: S3-compatible REST API

### Core Operations
- [ ] PUT object
- [ ] GET object
- [ ] DELETE object
- [ ] HEAD object
- [ ] LIST objects (v1 & v2)

### Bucket Operations
- [ ] PUT bucket
- [ ] DELETE bucket
- [ ] HEAD bucket
- [ ] LIST buckets

### Multipart Upload
- [ ] Initiate multipart upload
- [ ] Upload part
- [ ] Complete multipart upload
- [ ] Abort multipart upload
- [ ] List parts

### Advanced Features
- [ ] Object versioning
- [ ] Object tagging
- [ ] Object metadata
- [ ] Lifecycle policies
- [ ] Bucket policies
- [ ] Server-side encryption

**Deliverables**:
- [ ] `src/s3/` - S3 API handlers
- [ ] S3 compatibility test suite
- [ ] MinIO mc client compatibility

---

## Phase 10: Admin API (Weeks 43-46)

**Goal**: Cluster management interface

### Topology Commands
- [ ] `add-node` - Add node to cluster
- [ ] `remove-node` - Remove node from cluster
- [ ] `list-nodes` - List all cluster nodes
- [ ] `node-info` - Node details

### Migration Commands
- [ ] `migration-status` - Show progress
- [ ] `migration-pause` - Pause migration
- [ ] `migration-resume` - Resume migration

### Monitoring
- [ ] Health checks
- [ ] Metrics collection
- [ ] Prometheus exporter
- [ ] Status dashboards

**Deliverables**:
- [ ] `src/admin/` - Admin API
- [ ] CLI client (`buckets-admin`)
- [ ] Real-time status updates

---

## Phase 11: Testing & Hardening (Weeks 47-52)

**Goal**: Production readiness

### Testing
- [ ] Unit test coverage >85%
- [ ] Integration tests
- [ ] Chaos testing (node failures)
- [ ] Network partition tests
- [ ] Scale tests (100 nodes, 100M objects)
- [ ] Performance regression tests

### Tools
- [ ] Valgrind (memory leaks)
- [ ] AddressSanitizer (memory safety)
- [ ] ThreadSanitizer (race conditions)
- [ ] Fuzzing (AFL, libFuzzer)

### Documentation
- [ ] API documentation
- [ ] Admin guide
- [ ] Operations runbook
- [ ] Deployment guide
- [ ] Troubleshooting guide

### Performance
- [ ] Profile hotspots (perf, gprof)
- [ ] Optimize critical paths
- [ ] Memory usage optimization
- [ ] CPU usage optimization

**Deliverables**:
- [ ] Production-ready v1.0.0
- [ ] Comprehensive documentation
- [ ] Performance benchmarks

---

## Future Phases (Post v1.0)

### Phase 12: Advanced Features
- [ ] Replication
- [ ] Tiering (hot/cold storage)
- [ ] Compression
- [ ] Deduplication
- [ ] Object lock
- [ ] Legal hold

### Phase 13: Cloud Integration
- [ ] S3 gateway mode
- [ ] Azure Blob gateway
- [ ] GCS gateway
- [ ] Site replication

### Phase 14: Performance Enhancements
- [ ] DPDK networking
- [ ] SPDK storage
- [ ] io_uring optimizations
- [ ] Zero-copy operations
- [ ] GPU erasure coding

---

## Milestones

| Milestone | Target Date | Status |
|-----------|-------------|--------|
| **M1: Foundation** | Week 4 | 🔄 In Progress |
| **M2: Core Complete** | Week 11 | ⏳ Planned |
| **M3: Storage Ready** | Week 20 | ⏳ Planned |
| **M4: Registry Live** | Week 24 | ⏳ Planned |
| **M5: Migration Works** | Week 30 | ⏳ Planned |
| **M6: S3 Compatible** | Week 42 | ⏳ Planned |
| **M7: v1.0 Release** | Week 52 | ⏳ Planned |

---

## Success Metrics

### Performance Targets
- **Read Latency**: <5ms (cache hit <100ns)
- **Write Latency**: <50ms
- **Throughput**: >10 GB/s on 10GbE
- **Migration Speed**: >500 MB/s
- **Memory Usage**: <100MB per node

### Scalability Targets
- **Node Addition**: <1 minute (excluding migration)
- **Migration Impact**: <20% throughput degradation
- **Data Movement**: ~20% per node change
- **Cluster Size**: Support 100+ nodes

### Reliability Targets
- **Availability**: 99.99%
- **Data Durability**: 99.999999999% (11 nines)
- **Zero Data Loss**: During all topology changes
- **MTTR**: <5 minutes for single node failure

---

## Contributing

See current phase tasks and pick an item to work on. Each component has:
- Design specification
- Unit test requirements
- Performance targets
- Code review checklist

Join us in building the next generation of object storage!

---

**Last Updated**: February 25, 2026  
**Current Focus**: Phase 1 - Foundation
