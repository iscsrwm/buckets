<p align="center">
  <img src="images/BucketsLogoDark.png" alt="Buckets Logo" width="600"/>
</p>

# Buckets

A high-performance, S3-compatible object storage system written in C with support for fine-grained scalability.

## Overview

Buckets is a complete rewrite of object storage architecture that implements:

- **Fine-Grained Scalability**: Add/remove individual nodes (1-2 at a time) without cluster downtime
- **Location Registry**: Self-hosted metadata tracking for optimal read performance
- **Consistent Hashing**: Virtual node ring for minimal data migration during topology changes
- **S3 Compatibility**: Full Amazon S3 API compatibility
- **Erasure Coding**: Data protection with configurable redundancy
- **High Performance**: Written in C for maximum efficiency

## Architecture

Buckets implements a hybrid architecture combining:

1. **Location Registry** - Explicit object location tracking for <5ms reads
2. **Consistent Hashing** - Deterministic placement with ~20% migration on topology changes
3. **Topology Management** - Dynamic cluster membership with generation tracking
4. **Background Migration** - Automatic rebalancing when nodes are added/removed

See [architecture/SCALE_AND_DATA_PLACEMENT.md](architecture/SCALE_AND_DATA_PLACEMENT.md) for detailed design documentation.

## Key Features

- ✅ **Dynamic Node Management**: Add/remove nodes individually
- ✅ **Zero Downtime Operations**: Topology changes don't require restarts
- ✅ **Optimized Reads**: Direct lookup via registry (5-50× faster than multi-pool fan-out)
- ✅ **Controlled Migration**: ~20-30% data movement per node change
- ✅ **Fault Tolerant**: Graceful degradation, automatic recovery
- ✅ **Self-Contained**: No external dependencies (etcd, ZooKeeper, etc.)

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

### Single Node

```bash
# Start server
buckets server /data

# Create a bucket
buckets-client mb local/mybucket

# Upload object
buckets-client cp myfile.txt local/mybucket/
```

### Distributed Cluster

```bash
# Start 4-node cluster
buckets server http://node{1...4}:9000/data{1...4}
```

### Add Node Dynamically

```bash
# Add individual node to existing cluster
buckets-admin cluster add-node http://node5:9000/data{1...4} --pool 0

# Check migration status
buckets-admin cluster migration-status
```

## Project Status

**Current Phase**: Phase 3 - Cryptography & Erasure Coding (Week 8)  
**Progress**: 2 of 11 phases complete (18%)

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

### Current Stats

- **Production Code**: 3,501 lines (2,581 foundation + 920 hashing)
- **Test Code**: 2,085 lines
- **Test Coverage**: 111/111 tests passing (100%)
- **Build**: Clean with `-Wall -Wextra -Werror -pedantic`

### Next Up

- Week 8: BLAKE2b cryptographic hashing
- Week 9-11: SHA-256, bitrot detection, Reed-Solomon erasure coding

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
│   ├── crypto/           # Cryptography (Week 8+)
│   ├── erasure/          # Erasure coding (Week 8-11)
│   ├── storage/          # Storage layer (Week 12-16)
│   ├── registry/         # Location registry (Week 17-20)
│   ├── migration/        # Data rebalancing (Week 21-24)
│   ├── net/              # Network layer (Week 25-28)
│   ├── s3/               # S3 API handlers (Week 29-40)
│   └── admin/            # Admin API (Week 41-44)
├── include/              # Public headers ✅
│   ├── buckets.h         # Main API
│   ├── buckets_cluster.h # Cluster structures
│   ├── buckets_hash.h    # Hash algorithms
│   ├── buckets_ring.h    # Hash ring API
│   └── ...               # I/O, JSON, cache, endpoint headers
├── tests/                # Unit and integration tests ✅
│   ├── cluster/          # 60 tests (format, topology, endpoint)
│   └── hash/             # 49 tests (siphash, xxhash, ring)
├── docs/                 # Documentation
│   └── PROJECT_STATUS.md # Detailed progress tracking
├── architecture/         # Design documents
│   └── SCALE_AND_DATA_PLACEMENT.md  # 75-page architecture spec
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
# All tests (111 tests)
make test

# Specific component tests
make test-format      # Format management (20 tests)
make test-topology    # Topology management (18 tests)
make test-endpoint    # Endpoint parsing (22 tests)

# Run specific test binary
./build/test/hash/test_siphash    # SipHash tests (16 tests)
./build/test/hash/test_xxhash     # xxHash tests (16 tests)
./build/test/hash/test_ring       # Hash ring tests (17 tests)

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
