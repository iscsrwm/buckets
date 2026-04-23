# Performance Optimization Plan

**Date**: April 22, 2026  
**Current Performance**: 160 ops/sec (256KB objects, 3 concurrent clients)  
**Target**: 500+ ops/sec

## Current Performance Analysis

### Benchmark Results

**Multi-Client Test (3 clients, 20 workers each)**:
- Aggregate throughput: 160.65 ops/sec
- Success rate: 100%
- Average latency: 314-462ms per client

### Server Metrics

**From instrumentation**:
```
Thread Pool: queue_depth=0 (no saturation)
Thread Pool Wait: avg=3-4ms (excellent)
Request Latency: avg=4-5ms HTTP layer
Errors: timeouts=0, parse=0, write=0
```

**Key Insight**: Thread pool is NOT saturated. The bottleneck is elsewhere.

### Operation Breakdown (256KB PUT)

Based on logs and architecture:

1. **HTTP Layer**: ~5ms
   - Request parsing
   - Body streaming
   - Response generation

2. **Erasure Encoding**: Est. 2-5ms
   - 256KB → 8 data chunks (32KB each)
   - 4 parity chunks
   - ISA-L library (highly optimized)

3. **Parallel Chunk Write**: Est. 300-400ms (BOTTLENECK)
   - 12 chunks written in parallel
   - Each chunk: RPC call + network + disk I/O
   - Dominated by slowest chunk

4. **Metadata Write**: Est. 10-20ms
   - Write xl.meta to 12 disks in parallel
   - Smaller payload than chunks

**Total**: ~320-430ms (matches observed 314-462ms latency!)

### Bottleneck Identification

**The primary bottleneck is: Disk I/O + Network RPC for chunk writes**

Evidence:
- Thread pool not saturated (3-4ms wait time)
- Erasure encoding fast (<5ms based on ISA-L benchmarks)
- HTTP layer fast (4-5ms measured)
- **Chunk writes take 300-400ms** (dominant factor)

Why chunk writes are slow:
1. **12 parallel network RPCs** - Even though parallel, limited by slowest
2. **Disk fsync per write** - Despite group commit, still overhead
3. **Network latency** - RPC round-trip time adds up

## Optimization Opportunities

### 1. ⭐ io_uring Async I/O (HIGHEST IMPACT)

**Current**: Blocking I/O in libuv thread pool
```c
// Thread blocks during disk I/O
read(fd, buffer, size);  // Thread sleeps here
write(fd, buffer, size); // Thread sleeps here
fsync(fd);               // Thread sleeps here
```

**Problem**: Even with 512 threads, blocking is inefficient

**Solution**: io_uring for true async I/O
```c
// Submit I/O, thread continues
io_uring_prep_write(sqe, fd, buffer, size);
io_uring_submit(ring);
// ... do other work ...
// Later: check completion
io_uring_wait_cqe(ring, &cqe);
```

**Benefits**:
- Eliminate thread blocking
- Better CPU utilization
- More concurrent I/O operations
- Reduced context switching

**Expected Impact**: 2-3x throughput improvement (160 → 320-480 ops/sec)

**Effort**: High (3-5 days implementation + testing)

**Implementation Plan**:
1. Add io_uring to dependencies
2. Create io_uring wrapper API
3. Replace chunk write operations
4. Replace metadata write operations
5. Benchmark and tune

### 2. RPC Batching (MEDIUM IMPACT)

**Current**: 12 individual RPC calls for chunk writes

**Optimization**: Batch multiple chunks in single RPC
```c
// Instead of 12 individual calls
for (i = 0; i < 12; i++) {
    rpc_write_chunk(chunk[i]);
}

// Batch into fewer calls
rpc_write_chunk_batch(chunks, 12);  // 1 call instead of 12
```

**Benefits**:
- Reduce network round-trips
- Less RPC overhead
- Better pipelining

**Expected Impact**: 20-30% improvement

**Effort**: Medium (2-3 days)

### 3. Optimize Group Commit Parameters (LOW EFFORT, MEDIUM IMPACT)

**Current**: Group commit already enabled

**Optimization**: Tune parameters
- Batch size
- Batch timeout
- Write-ahead logging

**Expected Impact**: 10-20% improvement

**Effort**: Low (1 day tuning + testing)

### 4. Smart Caching Layer (MEDIUM IMPACT, GET-focused)

**Current**: Every GET reads from disk

**Optimization**: LRU cache for hot objects
- In-memory cache for metadata
- Optional data caching for small objects

**Expected Impact**: 
- Minimal impact on PUT
- 2-5x improvement on GET for cached objects

**Effort**: Medium (3-4 days)

### 5. Load Balancer Fix (INVESTIGATE)

**Issue**: LoadBalancer with sessionAffinity performed poorly (0.23-0.49 ops/sec)

**Needs Investigation**:
- Why did sessionAffinity cause issues?
- Can we use round-robin instead?
- What's the actual failure mode?

**Potential Impact**: 6x if we can utilize all 6 pods properly

**Effort**: Low (investigate) + unknown (fix)

## Recommended Approach

### Phase 1: Investigation & Quick Wins (Week 1)

1. **Investigate LoadBalancer issue** (Day 1-2)
   - Understand why sessionAffinity failed
   - Test with sessionAffinity: None
   - If fixable: **Instant 6x improvement** by using all pods

2. **Tune Group Commit** (Day 3)
   - Optimize batch size/timeout
   - Benchmark different configurations
   - Expected: 10-20% improvement

3. **Profile I/O patterns** (Day 4-5)
   - Confirm disk I/O is bottleneck
   - Measure actual fsync times
   - Validate io_uring would help

### Phase 2: io_uring Implementation (Week 2-3)

1. **Setup io_uring infrastructure** (Week 2)
   - Add liburing dependency
   - Create async I/O wrapper API
   - Unit tests for async operations

2. **Integrate with storage layer** (Week 3)
   - Replace chunk writes
   - Replace metadata writes
   - Integration testing

3. **Benchmark and tune** (Week 3)
   - Expected: 2-3x improvement
   - Target: 320-480 ops/sec

### Phase 3: Advanced Optimizations (Week 4+)

1. **RPC Batching**
2. **Smart Caching**
3. **Additional tuning**

## Success Metrics

| Metric | Current | Phase 1 Target | Phase 2 Target | Phase 3 Target |
|--------|---------|----------------|----------------|----------------|
| Throughput | 160 ops/sec | 200 ops/sec | 400 ops/sec | 500+ ops/sec |
| Latency (avg) | 380ms | 300ms | 150ms | 100ms |
| Success Rate | 100% | 100% | 100% | 100% |

## Next Steps

**IMMEDIATE** (Today):
1. Investigate LoadBalancer sessionAffinity issue
   - Test with `sessionAffinity: None`
   - Understand why current setup fails
   - Potentially unlock 6x improvement

2. If LoadBalancer works → 900+ ops/sec possible immediately!
3. If LoadBalancer doesn't work → Proceed with io_uring plan

**This investigation is CRITICAL before investing in io_uring.**

---

**Status**: Ready to proceed with Phase 1  
**Blockers**: None  
**Risk**: Low (incremental improvements)
