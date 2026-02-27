<p align="center">
  <img src="images/BucketsLogoDark.png" alt="Buckets Logo" width="600"/>
</p>

# Buckets

A high-performance, S3-compatible object storage system written in C with support for fine-grained scalability.

## Overview

Buckets is a complete rewrite of object storage architecture that implements:

- **Erasure Set Scaling**: Scale by adding complete erasure sets (not individual nodes)
- **Parallel RPC**: Concurrent chunk operations across multiple nodes for high throughput
- **Location Registry**: Self-hosted metadata tracking for optimal read performance
- **Consistent Hashing**: Virtual node ring for minimal data migration (~33% per set addition)
- **S3 Compatibility**: Full Amazon S3 API compatibility
- **Erasure Coding**: Data protection with configurable redundancy (K+M chunks)
- **High Performance**: Written in C with zero-copy I/O and connection pooling

## Scaling Methodology

**Important**: Buckets scales by adding **complete erasure sets**, not individual nodes.

### Erasure Set Architecture

An **erasure set** is a group of disks that work together for erasure coding:
- **Configuration**: K data shards + M parity shards = total disks per set
- **Example**: K=8, M=4 requires 12 disks per erasure set
- **Distribution**: Disks can span multiple physical nodes

### Scaling Examples

#### Small Cluster (3 nodes, 1 set)
```
Configuration: K=2, M=2 (4 disks total)
- Node 1: 2 disks → Set 0 (disks 0-1)
- Node 2: 1 disk  → Set 0 (disk 2)
- Node 3: 1 disk  → Set 0 (disk 3)

Fault tolerance: Survives any 2 disk failures
```

#### Medium Cluster (6 nodes, 2 sets)
```
Configuration: K=8, M=4 (12 disks per set, 24 total)
- Nodes 1-3: 12 disks → Set 0 (disks 0-11)
- Nodes 4-6: 12 disks → Set 1 (disks 12-23)

Capacity: 2× the small cluster
Fault tolerance: Each set survives any 4 disk failures
```

#### Large Cluster (9 nodes, 3 sets)
```
Configuration: K=8, M=4 (12 disks per set, 36 total)
- Nodes 1-3: 12 disks → Set 0
- Nodes 4-6: 12 disks → Set 1
- Nodes 7-9: 12 disks → Set 2

Capacity: 3× the small cluster
Data migration when adding Set 2: ~33% of objects rebalanced
```

### Why Erasure Sets?

1. **Consistent Performance**: All sets have the same K+M configuration
2. **Predictable Fault Tolerance**: Each set independently survives M disk failures
3. **Minimal Migration**: Adding a set migrates ~1/N objects (N = number of sets)
4. **Simplified Operations**: No partial sets or complex rebalancing logic

### Consistent Hashing for Placement

- Each erasure set gets 150 virtual nodes on the hash ring
- Objects are placed deterministically based on object name hash
- Adding a new set: ~33% migration (from 2 sets to 3 sets = 1/3 of objects move)
- Removing a set: Objects redistributed evenly across remaining sets

## Architecture

Buckets implements a distributed architecture with:

1. **Parallel RPC Layer** - Concurrent chunk writes/reads across nodes for high throughput
2. **Topology Management** - Disk endpoint mapping for cross-node distribution
3. **Consistent Hashing** - Virtual node ring for deterministic object→set mapping
4. **Location Registry** - Explicit object location tracking for <5ms reads
5. **Background Migration** - Automatic rebalancing when erasure sets are added/removed

See [architecture/SCALE_AND_DATA_PLACEMENT.md](architecture/SCALE_AND_DATA_PLACEMENT.md) for detailed design documentation.

## Key Features

- ✅ **Erasure Set Scaling**: Add complete sets (K+M disks) for predictable capacity growth
- ✅ **Parallel RPC Operations**: Concurrent chunk I/O across nodes for maximum throughput
- ✅ **Optimized Reads**: Direct lookup via registry (<5ms) + parallel chunk retrieval
- ✅ **Controlled Migration**: ~33% data movement per erasure set addition (1/N where N = sets)
- ✅ **Fault Tolerant**: Each set survives M disk failures independently
- ✅ **Self-Contained**: No external dependencies (etcd, ZooKeeper, Cassandra, etc.)
- ✅ **High Performance**: C implementation with connection pooling and zero-copy I/O

## Building from Source

### Prerequisites

```bash
# Required
gcc >= 9.0
make >= 4.0
cmake >= 3.15

# Libraries
libssl-dev      # Cryptography
zlib1g-dev      # Compression
libuuid-dev     # UUID generation
```

### Build

```bash
make
```

### Install

```bash
sudo make install
```

## Quick Start

### Single Node (Development)

```bash
# Start server on default port 9000
./bin/buckets server --port 9000

# Upload using curl
curl -X PUT -T myfile.txt http://localhost:9000/mybucket/myfile.txt

# Download
curl http://localhost:9000/mybucket/myfile.txt
```

### Distributed Cluster (Production)

#### 1. Format Disks (Run Once)

```bash
# Format node1 (4 disks)
./bin/buckets format --config config/cluster-node1.json

# Format node2 (4 disks)
./bin/buckets format --config config/cluster-node2.json

# Format node3 (4 disks)  
./bin/buckets format --config config/cluster-node3.json
```

#### 2. Start Cluster Nodes

```bash
# Start node1
./bin/buckets server --config config/cluster-node1.json &

# Start node2
./bin/buckets server --config config/cluster-node2.json &

# Start node3
./bin/buckets server --config config/cluster-node3.json &
```

#### 3. Use S3 API

```bash
# Upload to any node (automatically distributed)
curl -X PUT -T file.bin http://localhost:9001/bucket/file.bin

# Download from any node (chunks retrieved in parallel)
curl http://localhost:9002/bucket/file.bin -o downloaded.bin

# Verify integrity
md5sum file.bin downloaded.bin
```

### Scaling the Cluster

To add capacity, add a complete erasure set (3 more nodes with 12 disks):

```bash
# Create new configuration with 2 erasure sets
# Edit config files to include nodes 1-6, sets=2, disks_per_set=12

# Format new nodes (4-6)
./bin/buckets format --config config/cluster-node4.json
./bin/buckets format --config config/cluster-node5.json
./bin/buckets format --config config/cluster-node6.json

# Restart all nodes with updated config
# Automatic migration will redistribute ~33% of objects to new set
```

## Project Status

**Current Phase**: Phase 9 - S3 API Layer (Weeks 35-42) 🔄 In Progress  
**Progress**: 8 phases complete, Week 35 of 52 (67%)

### Completed

- ✅ **Phase 1: Foundation (Weeks 1-4)** - 100% Complete
  - Core utilities (memory, logging, strings)
  - Format management (format.json)
  - Topology management with caching
  - Endpoint parsing with ellipses expansion
  - 62 tests passing

- ✅ **Phase 2: Hashing (Weeks 5-7)** - 100% Complete
  - SipHash-2-4 cryptographic hashing
  - xxHash-64 fast non-cryptographic hashing
  - Hash ring with consistent hashing (150 virtual nodes)
  - Jump Consistent Hash
  - 49 tests passing

- ✅ **Phase 3: Cryptography & Erasure (Weeks 8-11)** - 100% Complete
  - BLAKE2b cryptographic hashing (1.6x faster than SHA-256)
  - SHA-256 (OpenSSL wrapper)
  - Reed-Solomon erasure coding with Intel ISA-L
  - 8+4, 12+4, 16+4 configurations tested
  - Automatic chunk reconstruction
  - 36 tests passing

- ✅ **Phase 4: Storage Layer (Weeks 12-16)** - 100% Complete
  - ✅ Object primitives & disk I/O (Week 12)
  - ✅ Object metadata & versioning (Week 13)
  - ✅ Multi-disk management & healing (Week 14-16)
  - MinIO-compatible xl.meta format
  - S3-compatible versioning with delete markers
  - Quorum-based reads/writes (N/2+1)
  - Automatic healing of inconsistent metadata
  - LRU metadata cache (10K entries)
  - Performance benchmarks: 5-10 GB/s encode, 27-51 GB/s decode
  - 33 tests passing

- ✅ **Phase 5: Location Registry (Weeks 17-20)** - 100% Complete
  - ✅ Registry core implementation (Week 17)
  - ✅ Batch operations & benchmarks (Week 18)
  - ✅ Storage layer fixes (Week 19)
  - ✅ Production integration (Week 20)
  - Thread-safe LRU cache (1M entries, 5-min TTL)
  - Write-through cache with persistent storage
  - Self-hosted on Buckets (.buckets-registry bucket)
  - Automatic tracking of PUT/GET/DELETE operations
  - Cache hit latency: 0.323 μs
  - 15 tests passing (100%)

- ✅ **Phase 6: Topology Management (Weeks 21-24)** - 100% Complete
  - ✅ Dynamic topology operations (Week 21)
    - Add pool, add set, state transitions
    - Generation tracking
    - 8 tests passing
  - ✅ Quorum persistence (Week 22)
    - Write quorum (N/2+1 disks)
    - Read quorum with consensus (N/2 matching)
    - 12 tests passing
  - ✅ Topology manager API (Week 23)
    - Singleton coordination layer
    - Automatic quorum persistence
    - Event callbacks with user data
    - 11 tests passing
  - ✅ Integration testing (Week 24)
    - End-to-end topology change workflows
    - Critical bug fixes (pool count tracking)
    - 10 integration tests passing

- ✅ **Phase 7: Background Migration (Weeks 25-30)** - 100% Complete
  - ✅ Migration scanner (Week 25) - 10 tests passing
  - ✅ Worker pool (Week 26) - 12 tests passing
  - ✅ Migration orchestrator (Week 27) - 14 tests passing
  - ✅ Throttling (Week 28) - 15 tests passing
  - ✅ Checkpointing (Week 29) - 10 tests passing
  - ✅ Integration & Recovery (Week 30) - 10 tests passing
  - **Total**: 71 tests, 100% passing

- ✅ **Phase 8: Network Layer (Weeks 31-34)** - 100% Complete
  - ✅ HTTP Server Foundation (Week 31)
    - Mongoose library integration (HTTP/1.1 server)
    - Thread-based event loop (100ms polling)
    - Request router with pattern matching
    - Response helpers (JSON, errors, headers)
    - 21 tests passing (100%): 13 HTTP server + 11 router - 3 TLS
  - ✅ TLS & Connection Pooling (Week 32)
    - OpenSSL TLS support via mongoose
    - Connection pool for peer communication
    - Connection lifecycle management
    - 13 tests passing (100%): 3 TLS + 10 connection pool
  - ✅ Peer Discovery & Health (Week 33)
    - Peer grid with UUID-based node IDs
    - Health checker with periodic heartbeats
    - Background thread for monitoring
    - 10 tests passing (100%): peer grid
  - ✅ RPC & Broadcast (Week 34)
    - JSON-based RPC message format
    - Handler registration and dispatch
    - Broadcast to all peers in grid
    - 18 tests passing (100%): 12 RPC + 6 broadcast
  - **Total**: 62 tests, 100% passing

- 🔄 **Phase 9: S3 API Layer (Weeks 35-42)** - 12.5% Complete (Week 35/42)
  - ✅ S3 PUT/GET Operations (Week 35)
    - XML response generation (success/error)
    - AWS Signature V4 authentication framework
    - PUT/GET/DELETE/HEAD object operations
    - ETag calculation (MD5 hashing)
    - Bucket/key validation
    - File system storage backend (/tmp/buckets-data/)
    - 12 tests passing (100%): 5 XML + 7 operations
  - ⏳ Bucket Operations (Week 37) - Planned
  - ⏳ LIST Objects (Week 38) - Planned
  - ⏳ Multipart Upload (Weeks 39-40) - Planned
  - ⏳ Versioning & Metadata (Week 41) - Planned
  - ⏳ Integration Testing (Week 42) - Planned

### Current Stats

- **Production Code**: ~19,597 lines
  - Core: 255 lines
  - Cluster: 3,050 lines (+420 manager)
  - Hash: 920 lines
  - Crypto: 527 lines
  - Erasure: 546 lines
  - Storage: 4,171 lines
  - Registry: 1,266 lines
  - Migration: 2,222 lines
  - Network: 4,484 lines (server 361+58, router 179, pool 432, grid 326, health 305, rpc 552, broadcast 150, header 725, mongoose 991KB)
  - S3 API: 1,252 lines (xml 195, auth 374, handler 293, ops 390)
  - Benchmarks: 618 lines
- **Test Code**: ~10,542 lines
- **Test Coverage**: 305/306 tests passing (99.7%)
  - Foundation: 62 tests
  - Hashing: 49 tests
  - Crypto & Erasure: 36 tests
  - Storage: 33 tests
  - Registry: 15 tests
  - Topology: 31 tests
  - Migration: 61 tests (scanner 10, worker 12, orchestrator 14, throttle 15, checkpoint 10)
  - Network: 62 tests (HTTP 13, router 11, pool 10, grid 10, RPC 12, broadcast 6)
  - S3 API: 12 tests (xml 5, operations 7)
  - Storage Integration: 5 tests
- **Build**: Clean with `-Wall -Wextra -Werror -pedantic`
- **Library Size**: ~340KB (includes ISA-L, mongoose)

### Performance Highlights

- **Erasure Coding**: 5-10 GB/s encode, 27-51 GB/s decode (Intel ISA-L)
- **Hashing**: BLAKE2b 880 MB/s (1.6x faster than SHA-256)
- **Reconstruction**: 31-52 GB/s with missing disks
- **Registry Lookups**: 0.323 μs cache hit, ~1-5ms cache miss
- **RPC Latency**: <10ms for local peers
- **Broadcast**: <100ms to 10 peers

### Next Up

- Week 35: S3 API Layer (GET/PUT operations)
  - Implement S3 bucket and object operations
  - XML request/response parsing
  - Authentication and authorization
  - Integration with storage layer

See [ROADMAP.md](ROADMAP.md) for detailed development timeline and [docs/PROJECT_STATUS.md](docs/PROJECT_STATUS.md) for comprehensive progress tracking.

## Comparison with MinIO

Buckets is based on MinIO's architecture but with significant enhancements:

| Feature | MinIO | Buckets |
|---------|-------|---------|
| **Language** | Go | C |
| **Node Scalability** | Pool-level only | Individual nodes |
| **Read Latency (multi-pool)** | 10-50ms | <5ms |
| **Migration on Scale** | Add full pool | ~20% of data |
| **Dynamic Topology** | Requires restart | Zero downtime |
| **Memory Footprint** | ~500MB+ | Target <100MB |

## Documentation

- [Architecture Design](architecture/SCALE_AND_DATA_PLACEMENT.md) - Comprehensive technical specification
- [API Reference](docs/api/README.md) - S3 API compatibility
- [Admin Guide](docs/admin/README.md) - Cluster management
- [Developer Guide](docs/dev/README.md) - Contributing to Buckets

## Development

### Project Structure

```
buckets/
├── src/                   # Source code
│   ├── core/             # Core utilities ✅
│   ├── cluster/          # Cluster management ✅
│   │   ├── format.c      # format.json management
│   │   ├── topology.c    # topology.json management
│   │   ├── cache.c       # Thread-safe caching
│   │   ├── endpoint.c    # Endpoint parsing
│   │   └── ...           # Atomic I/O, disk utils, JSON helpers
│   ├── hash/             # Hashing algorithms ✅
│   │   ├── siphash.c     # SipHash-2-4 (cryptographic)
│   │   ├── xxhash.c      # xxHash-64 (fast)
│   │   └── ring.c        # Consistent hash ring
│   ├── crypto/           # Cryptography ✅
│   │   ├── blake2b.c     # BLAKE2b hashing
│   │   └── sha256.c      # SHA-256 wrapper
│   ├── erasure/          # Erasure coding ✅
│   │   └── erasure.c     # Reed-Solomon (ISA-L)
│   ├── storage/          # Storage layer ✅
│   │   ├── layout.c      # Path computation, chunk sizing
│   │   ├── metadata.c    # xl.meta serialization
│   │   ├── chunk.c       # Chunk I/O, checksums
│   │   ├── object.c      # PUT/GET/DELETE/HEAD/STAT
│   │   ├── versioning.c  # S3-compatible versioning
│   │   ├── metadata_cache.c  # LRU metadata cache
│   │   └── multidisk.c   # Multi-disk quorum operations
│   ├── registry/         # Location registry 🔄
│   │   └── registry.c    # Self-hosted location tracking
│   ├── migration/        # Data rebalancing (Week 21-24)
│   ├── net/              # Network layer (Week 25-28)
│   ├── s3/               # S3 API handlers (Week 29-40)
│   └── admin/            # Admin API (Week 41-44)
├── include/              # Public headers ✅
│   ├── buckets.h         # Main API
│   ├── buckets_cluster.h # Cluster structures
│   ├── buckets_hash.h    # Hash algorithms
│   ├── buckets_ring.h    # Hash ring API
│   ├── buckets_crypto.h  # Cryptographic hashing
│   ├── buckets_erasure.h # Erasure coding
│   ├── buckets_storage.h # Storage layer
│   └── buckets_registry.h # Location registry
├── tests/                # Unit and integration tests ✅
│   ├── cluster/          # 80 tests (format, topology, endpoint, cache, manager, integration)
│   ├── hash/             # 49 tests (siphash, xxhash, ring)
│   ├── crypto/           # 28 tests (blake2b, sha256)
│   ├── erasure/          # 20 tests (reed-solomon)
│   ├── storage/          # 33 tests (object, versioning, multidisk)
│   └── registry/         # 8 tests (simple, storage integration)
├── docs/                 # Documentation
│   └── PROJECT_STATUS.md # Detailed progress tracking
├── architecture/         # Design documents
│   ├── SCALE_AND_DATA_PLACEMENT.md  # 75-page architecture spec
│   ├── CLUSTER_AND_STATE_MANAGEMENT.md  # Cluster topology
│   ├── CRYPTOGRAPHY_AND_INTEGRITY.md    # Hashing and checksums
│   ├── STORAGE_LAYER.md                 # xl.meta format, erasure coding
│   └── LOCATION_REGISTRY_IMPLEMENTATION.md  # Registry implementation guide
└── third_party/          # Third-party libraries
    └── cJSON/            # JSON library
```

### Building Components

```bash
# Build core libraries
make core

# Build with tests
make test

# Build with debug symbols
make DEBUG=1

# Build specific component
make registry
```

### Running Tests

```bash
# All tests (242 tests)
make test

# Specific component tests
make test-format              # Format management (20 tests)
make test-topology            # Topology core (18 tests)
make test-topology-operations # Topology operations (8 tests)
make test-topology-quorum     # Quorum persistence (12 tests)
make test-topology-manager    # Topology manager (11 tests)
make test-topology-integration # Integration tests (9 tests)
make test-endpoint            # Endpoint parsing (22 tests)
make test-hash                # Hash algorithms (49 tests)
make test-crypto              # Cryptography (28 tests)
make test-erasure             # Erasure coding (20 tests)

# Run specific test binary
./build/test/registry/test_registry_simple      # Registry simple (5 tests)
./build/test/registry/test_registry_storage     # Registry storage (3 tests)
./build/test/storage/test_multidisk_integration # Multi-disk (10 tests)

# Performance benchmarks
make benchmark        # Run Phase 4 benchmarks

# With valgrind (memory leak detection)
make test-valgrind
```

## Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

### Development Principles

1. **Performance First**: C gives us control - use it wisely
2. **Memory Safety**: Use valgrind, AddressSanitizer, static analysis
3. **Testability**: Every component has unit tests
4. **Documentation**: Code is read more than written
5. **Incremental Progress**: Small, reviewable changes

## License

GNU Affero General Public License v3.0 (AGPLv3)

See [LICENSE](LICENSE) for details.

## Credits

Buckets is inspired by and references the [MinIO](https://github.com/minio/minio) project.

## Community

- **Issues**: [GitHub Issues](https://github.com/yourusername/buckets/issues)
- **Discussions**: [GitHub Discussions](https://github.com/yourusername/buckets/discussions)
- **Security**: See [SECURITY.md](SECURITY.md)

---

**Status**: Active Development  
**Version**: 0.1.0-alpha  
**Started**: February 2026
