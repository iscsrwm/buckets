# Buckets Performance Optimization Plan
## Scaling to 1,000s+ ops/sec on High-Speed Networks

**Document Version**: 1.0  
**Created**: April 21, 2026  
**Status**: 🔄 In Progress - Phase 1  
**Owner**: Performance Team  
**Target Completion**: Week 49 (June 2026)

---

## Executive Summary

### Current State
- **Baseline Performance**: 150 ops/sec (concurrent upload, localhost)
- **Bottleneck**: Disk I/O saturation (24 virtual disks on 1 physical disk)
- **Network Utilization**: <5% on 1GbE (currently disk-bound)
- **Test Environment**: 6-node localhost cluster, single physical disk

### Target Goals
- **10GbE Networks**: 5,000-10,000 ops/sec
- **25GbE Networks**: 15,000-25,000 ops/sec
- **100GbE Networks**: 20,000-50,000 ops/sec
- **Scalability**: Support 10s-100s of nodes with linear performance scaling

### Strategic Approach
Five complementary optimization initiatives targeting different bottlenecks:

1. **Group Commit & fsync Optimization** (3-5x gain) - Quick win
2. **Async Replication Pipeline** (2-4x gain) - Architectural improvement
3. **Smart Request Routing** (1.5-2x GET gain) - Distributed intelligence
4. **Binary Protocol** (1.5-2x gain) - Network efficiency
5. **Zero-Copy I/O with io_uring** (3-5x gain) - Ultimate performance

### Timeline
- **Week 42**: Planning & Group Commit (Phase 1) ✅ In Progress
- **Weeks 43-44**: Async Replication Pipeline (Phase 2)
- **Week 45**: Smart Request Routing (Phase 3)
- **Week 46**: Binary Protocol (Phase 4)
- **Weeks 47-49**: Zero-Copy io_uring (Phase 5)

### Expected Outcomes
- **Immediate** (Week 42): 450-750 ops/sec (+3-5x)
- **Short-term** (Week 44): 900-3,000 ops/sec (+6-20x)
- **Medium-term** (Week 46): 2,000-12,000 ops/sec (+13-80x)
- **Long-term** (Week 49): 6,000-60,000 ops/sec (+40-400x)

---

## Current Baseline Metrics

**Test Configuration** (April 20, 2026):
- 6 nodes (localhost:9001-9006)
- 24 disks (4 per node, all on single physical disk)
- Erasure coding: K=8, M=4 (12 disks per set)
- RPC semaphore: 512 concurrent calls
- libuv thread pool: 4 threads

### Performance Profile

| Metric | Value | Status |
|--------|-------|--------|
| **Concurrent Upload** (256KB, 50 workers) | 150.83 ops/sec | 🔴 Disk I/O bound |
| **Concurrent Download** (256KB, 50 workers) | 79.74 ops/sec | 🟡 Good scaling |
| **Large Download** (10MB) | 5.10 ops/sec / 51.02 MB/s | 🟢 Excellent |
| **Network Utilization** | <5% | 🔴 Under-utilized |
| **CPU Usage** | 0% | 🟢 Not CPU-bound |
| **Load Average** | 31.78 | 🔴 I/O waiting |

### Identified Bottlenecks

1. **fsync() Overhead** (PRIMARY)
   - Current: 12 fsyncs per object write
   - Physical disk limit: ~200-500 fsyncs/sec
   - With 12 chunks: Max ~40 objects/sec theoretical
   - Files: `src/cluster/atomic_io.c:73`, `src/net/async_io.c:104`

2. **Sequential RPC ACK Waiting**
   - Chunks written in parallel, but each waits for ACK
   - Client blocks until all 12 chunks confirmed
   - No pipelining between operations

3. **HTTP Protocol Overhead**
   - ~240 bytes per chunk request/response
   - Text parsing for every request
   - No connection multiplexing

4. **Memory Copies**
   - User buffer → libuv buffer → socket (2 copies per chunk)
   - Socket → libuv buffer → user buffer (2 copies per chunk)
   - Total: 4 copies per chunk × 256KB = 1MB copied per 256KB written

5. **Suboptimal Routing**
   - Every GET forwarded to storage node
   - No awareness of local chunk availability
   - Unnecessary network hops

---

## Performance Initiative #1: Group Commit & fsync Optimization

**Status**: 🔄 In Progress (Week 42)

### Problem Statement
Every chunk write calls `fsync()` immediately, creating severe bottleneck:
- 12 chunks per object × fsync = 12 × 5-10ms = 60-120ms per object
- Physical disk fsync limit: 200-500/sec → Max 16-40 objects/sec
- Current code: `fsync(fd)` after every write

### Solution: Batched fsync with Group Commit

**Concept**: Buffer multiple writes, single fsync across batch

**Implementation**: Hybrid batching (count + time)
- Accumulate up to 64 writes OR 10ms window
- Single `fdatasync()` for entire batch (faster than fsync)
- Per-disk batching with thread-safe access

### Expected Results

| Metric | Current | Target | Improvement |
|--------|---------|--------|-------------|
| fsync calls per object | 12 | 0.12-1.2 | 10-100x reduction |
| Concurrent upload ops/sec | 150 | 450-750 | 3-5x |
| Upload latency p50 | 60ms | 20-30ms | 2-3x |

### Files Created/Modified

**New Files**:
- `src/storage/group_commit.c` (~300 lines) - Group commit buffer management
- `include/buckets_group_commit.h` (~50 lines) - API declarations
- `tests/storage/test_group_commit.c` (~200 lines) - Unit tests

**Modified Files**:
- `src/cluster/atomic_io.c` - Replace fsync with batched sync
- `src/net/async_io.c` - Integrate with libuv event loop

**Estimated Timeline**: 3-5 days (Week 42)

---

## Performance Initiative #2: Async Replication Pipeline

**Status**: ⏸️ Planned (Weeks 43-44)

### Problem Statement
Current synchronous flow blocks client until full replication (~100ms per object).

### Solution: Async Pipeline with Early ACK

**Key Concept**: Return to client after K data shards, complete M parity async

**Architecture**:
- Async operation queue
- Worker pool (8 threads) for replication
- 202 Accepted response to client
- Operation status API

### Expected Results

| Metric | Current | Target | Improvement |
|--------|---------|--------|-------------|
| Client wait time | 100ms | 30-40ms | 2.5-3x |
| Concurrent ops/sec | 150 | 300-600 | 2-4x |
| Pipeline depth | 1 | 10-50 | Can queue operations |

**Estimated Timeline**: 1-2 weeks (Weeks 43-44)

---

## Performance Initiative #3: Smart Request Routing

**Status**: ⏸️ Planned (Week 45)

### Problem Statement
Current routing is naive - every GET forwarded to single node, no awareness of local chunks.

### Solution: Distributed Query Optimization

**Strategy**:
- Check local chunk availability
- Local-first reads (0 network hops)
- Forward to optimal node
- Parallel distributed GET (wait for first K responses)

### Expected Results

| Metric | Current | Target | Improvement |
|--------|---------|--------|-------------|
| GET latency (local) | 30-40ms | 5-10ms | 3-4x |
| GET ops/sec | 80 | 120-160 | 1.5-2x |
| Network hops | 2-4 | 0-2 | 50-100% reduction |

**Estimated Timeline**: 1 week (Week 45)

---

## Performance Initiative #4: Binary Protocol (BBP)

**Status**: ⏸️ Planned (Week 46)

### Problem Statement
HTTP overhead for internal chunk transfers (~240 bytes + parsing overhead).

### Solution: Custom Binary Protocol

**Buckets Binary Protocol (BBP)**:
- 32-byte fixed header (vs 200+ byte HTTP)
- 5-byte response (vs 40+ byte HTTP)
- No text parsing
- CRC32 verification
- Request pipelining

### Expected Results

| Metric | HTTP | BBP | Improvement |
|--------|------|-----|-------------|
| Protocol overhead | 240 bytes | 37 bytes | 6.5x reduction |
| Parsing time | 15μs | <1μs | 15x faster |
| Ops/sec (10GbE) | 3,000 | 4,500 | 1.5x |
| Ops/sec (100GbE) | 12,000 | 24,000 | 2x |

**Estimated Timeline**: 1 week (Week 46)

---

## Performance Initiative #5: Zero-Copy I/O with io_uring

**Status**: ⏸️ Planned (Weeks 47-49)

### Problem Statement
Current I/O path has 4 memory copies per chunk (Disk → Kernel → libuv → User → Socket).

### Solution: Linux io_uring Zero-Copy Architecture

**Benefits**:
- Zero-copy transfers via splice/sendfile
- Batched syscalls (1 syscall for 100s of operations)
- Kernel polling mode (eliminates context switching)
- Direct DMA from NIC → disk

### Expected Results

| Metric | Current (libuv) | Target (io_uring) | Improvement |
|--------|-----------------|-------------------|-------------|
| Memory copies per chunk | 4 | 0 | ∞ |
| Syscalls per batch (12 ops) | 24-36 | 1-2 | 12-36x |
| CPU usage | 30% | 5-10% | 3-6x reduction |
| Ops/sec (100 nodes, 100GbE) | ~10,000 | 30,000-50,000 | 3-5x |

**Estimated Timeline**: 2-3 weeks (Weeks 47-49)

---

## Implementation Timeline

### Week 42: Group Commit (Phase 1) ✅ In Progress

**Day 1-2**: Planning & Setup
- ✅ Create performance plan document
- ✅ Review baseline metrics
- ⏳ Set up test harness

**Day 3-4**: Implementation
- ⏳ Create group_commit.c
- ⏳ Modify atomic_io.c and async_io.c
- ⏳ Implement hybrid batching

**Day 5**: Testing & Validation
- ⏳ Unit tests
- ⏳ Performance benchmarks
- ⏳ Update PROJECT_STATUS.md

### Week 43-44: Async Replication Pipeline (Phase 2)

**Week 43**:
- Async queue infrastructure
- Worker pool implementation
- S3 API integration

**Week 44**:
- Pipelining support
- Testing and validation
- Performance benchmarks

### Week 45: Smart Request Routing (Phase 3)

- Location cache implementation
- Smart routing logic
- Parallel GET support

### Week 46: Binary Protocol (Phase 4)

- Protocol implementation
- Server handler
- Integration and testing

### Week 47-49: Zero-Copy io_uring (Phase 5)

**Week 47**: io_uring core + zero-copy transfers
**Week 48**: Batched operations + kernel polling
**Week 49**: Advanced optimizations + final testing

---

## Success Metrics

### Phase Success Criteria

Each phase must meet:

1. **Correctness**
   - ✅ All existing unit tests pass
   - ✅ No data corruption in stress tests
   - ✅ Checksums verified

2. **Performance**
   - ✅ Minimum target improvement achieved
   - ✅ No regression in other operations

3. **Stability**
   - ✅ 24-hour stress test passes
   - ✅ No memory leaks (Valgrind clean)

4. **Documentation**
   - ✅ Performance report written
   - ✅ PROJECT_STATUS.md updated

### Overall Project Success

**Minimum Success** (Week 49):
- 10x improvement over baseline (150 → 1,500 ops/sec)
- Zero data corruption
- Production-ready on 10GbE networks

**Target Success** (Week 49):
- 40x improvement over baseline (150 → 6,000 ops/sec)
- 10GbE: 5,000-10,000 ops/sec
- Linear scaling to 100+ nodes

**Stretch Goal**:
- 100x improvement (150 → 15,000 ops/sec)
- 100GbE: 30,000-50,000 ops/sec

---

## Configuration Management

### Performance Configuration

Feature flags for gradual rollout:

```json
{
  "group_commit": {
    "enabled": true,
    "batch_size": 64,
    "batch_time_ms": 10,
    "use_fdatasync": true,
    "durability_level": "batched"
  },
  "async_replication": {
    "enabled": false,
    "worker_count": 8,
    "early_ack": true
  },
  "smart_routing": {
    "enable_local_reads": false,
    "cache_size": 10000
  },
  "binary_protocol": {
    "enable_bbp": false
  },
  "io_uring": {
    "enabled": false,
    "queue_depth": 256
  }
}
```

---

## Monitoring & Observability

### Key Performance Indicators

1. **Throughput**: PUT/GET/DELETE ops/sec
2. **Latency**: p50, p95, p99
3. **Resources**: CPU, memory, network, disk I/O
4. **System Health**: fsync rate, queue depth, cache hit rate

---

## Risk Assessment

### Technical Risks

| Risk | Probability | Impact | Mitigation |
|------|------------|--------|------------|
| io_uring kernel incompatibility | Medium | High | Feature detection, fallback |
| Data corruption | Low | Critical | Extensive testing, checksums |
| Performance not meeting targets | Medium | Medium | Incremental approach |
| Increased complexity | High | Medium | Documentation, code reviews |

---

## Next Steps

1. ✅ Create performance plan document
2. ⏳ Implement Phase 1: Group Commit
3. ⏳ Run baseline benchmarks
4. ⏳ Validate 3-5x improvement
5. ⏳ Update PROJECT_STATUS.md

---

**Document Status**: Living document, updated after each phase completion

**Last Updated**: April 21, 2026
