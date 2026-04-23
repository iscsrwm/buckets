# Batch Write Optimization

**Date**: April 22, 2026  
**Status**: ✅ Implemented, Ready for Testing  
**Expected Impact**: 30-50% throughput improvement

## Summary

Implemented batched chunk writes to reduce network round-trips for erasure-coded objects. Instead of sending 12 individual HTTP requests for a 12-chunk object, the system now groups chunks by destination node and sends them in batches.

## Problem Statement

### Before Optimization

For a typical erasure-coded object with K=8, M=4 (12 chunks total) distributed across 3 nodes:
- **12 individual HTTP requests** (one per chunk)
- Each request requires: TCP connection + HTTP headers + chunk data + response
- Network round-trip time dominates: ~50ms per chunk = 600ms total
- This creates a hard throughput ceiling of ~200-220 ops/sec

### Performance Bottleneck

From testing (April 22, 2026):
```
Current Performance:
- 16 workers: 211.92 ops/sec, 75.44ms avg latency
- 32 workers: 215.30 ops/sec, 148.39ms avg latency  
- 64 workers: 201.71 ops/sec, 316.29ms avg latency

Bottleneck Identified:
- Network/RPC latency for distributed chunk writes
- 12 HTTP requests per object × 50ms each = 600ms overhead
- Thread pool size doesn't help (tested 512 → 1024: no improvement)
- io_uring SQ_POLL doesn't help (syscalls not the bottleneck)
```

## Solution: Batched Chunk Writes

### Key Innovation

Group chunks by destination node and send them in a single HTTP request:

```
Example: 12 chunks across 3 nodes (4 chunks each)

WITHOUT batching:
  Node A: chunk 1 → HTTP request (50ms)
  Node A: chunk 2 → HTTP request (50ms)
  Node A: chunk 3 → HTTP request (50ms)
  Node A: chunk 4 → HTTP request (50ms)
  Node B: chunk 5 → HTTP request (50ms)
  ... (total 12 requests = 600ms)

WITH batching:
  Node A: chunks 1-4 → HTTP request (50ms)
  Node B: chunks 5-8 → HTTP request (50ms)
  Node C: chunks 9-12 → HTTP request (50ms)
  (total 3 requests = 150ms)

Network overhead: 600ms → 150ms (4x reduction)
```

## Implementation Details

### New Files Created

1. **`src/storage/batch_transport.c`** (~450 lines)
   - `buckets_binary_batch_write_chunks()` - Client-side batch write
   - `buckets_handle_batch_chunk_write()` - Server-side handler
   - Binary protocol for efficient batch transmission

2. **`src/storage/parallel_chunks_batched.c`** (~300 lines)
   - `buckets_batched_parallel_write_chunks()` - Groups chunks by node
   - Automatic batching logic (transparent to callers)
   - Falls back to non-batched for local-only mode

### Binary Batch Protocol

**Endpoint**: `PUT /_internal/batch_chunks`

**Headers**:
```http
Content-Type: application/x-buckets-batch
X-Batch-Count: N
Content-Length: <total_size>
```

**Body Format** (binary):
```
For each chunk:
  [4 bytes: chunk_index (u32, network order)]
  [4 bytes: chunk_size (u32, network order)]
  [256 bytes: bucket (null-terminated string)]
  [1024 bytes: object (null-terminated string)]
  [512 bytes: disk_path (null-terminated string)]
  [chunk_size bytes: chunk data]
```

### Integration Points

**Modified Files**:
- `src/storage/object.c` - Changed to use `buckets_batched_parallel_write_chunks()`
- `src/storage/binary_transport.c` - Registered batch handler, exposed helper functions
- `include/buckets_storage.h` - Added batch API declarations

**Connection Reuse**:
- Uses existing connection cache (64 connections, 30s keep-alive)
- Batch requests also benefit from cached connections
- No additional connection overhead

## Expected Performance Impact

### Theoretical Maximum

```
Before batching:
- 12 chunks × 50ms network RTT = 600ms per object
- Max throughput: 1000ms / 600ms ≈ 1.67 objects/sec per worker
- With 16 workers: 16 × 1.67 ≈ 27 ops/sec (theoretical)
- Actual: 211 ops/sec (with pipelining and concurrency)

After batching:
- 3 batches × 50ms network RTT = 150ms per object  
- Max throughput: 1000ms / 150ms ≈ 6.67 objects/sec per worker
- With 16 workers: 16 × 6.67 ≈ 107 ops/sec (theoretical)
- Expected actual: 280-320 ops/sec (30-50% improvement)
```

### Conservative Estimate

**Expected throughput**: 280-300 ops/sec (32% improvement)
- Base: 211 ops/sec
- Reduction in network overhead: 4x
- Real-world pipelining effects: ~32% net gain

## Testing Plan

### Unit Testing

```bash
# Test batch write with controlled chunks
./test_batch_write_local

# Test grouping logic
./test_batch_grouping
```

### Integration Testing

```bash
# Single-node cluster (should use batch writes)
./bin/buckets server --config config/node1.json

# Upload test objects
for i in {1..100}; do
  dd if=/dev/urandom of=/tmp/test_$i.dat bs=1M count=1
  curl -X PUT --data-binary @/tmp/test_$i.dat \
    http://localhost:9001/test-bucket/test_$i.dat
done

# Verify batch write logs
grep "\[BATCHED_WRITE\]" /var/log/buckets.log
grep "\[BATCH_WRITE\]" /var/log/buckets.log
```

### Performance Benchmarking

```bash
# Deploy to Kubernetes
cd k8s
./deploy.sh deploy

# Run benchmark (16 workers, 60s)
python3 benchmark.py --workers 16 --duration 60 --size 64KB

# Expected results:
# - Throughput: 280-300 ops/sec (up from 211)
# - Latency: 50-60ms avg (down from 75ms)
# - Success rate: 100%
```

## Logging and Observability

### Client-Side Logs

```
[BATCHED_WRITE] Starting batched write: 12 chunks for bucket/object
[BATCHED_WRITE] Grouped 12 chunks into 3 batches
[BATCHED_WRITE] Batch 0: 4 chunks → REMOTE http://node-1:9001
[BATCHED_WRITE] Batch 1: 4 chunks → REMOTE http://node-2:9002
[BATCHED_WRITE] Batch 2: 4 chunks → LOCAL local
[BATCH_WRITE] SUCCESS: 4 chunks, 1048576 bytes in 52.34 ms (19.04 MB/s) endpoint=http://node-1:9001
[BATCHED_WRITE] SUCCESS: All 3 batches completed (12 chunks total)
```

### Server-Side Logs

```
[BATCH_HANDLER] Receiving 4 chunks
[BATCH_HANDLER] chunk=1 size=262144 bucket=test object=file.dat disk=/data/disk1
[BATCH_HANDLER] chunk=2 size=262144 bucket=test object=file.dat disk=/data/disk2
[BATCH_HANDLER] chunk=3 size=262144 bucket=test object=file.dat disk=/data/disk3
[BATCH_HANDLER] chunk=4 size=262144 bucket=test object=file.dat disk=/data/disk4
[BATCH_HANDLER] Wrote 4/4 chunks successfully
```

## Rollback Plan

If issues arise, revert to non-batched writes:

```c
// In src/storage/object.c, change line ~560:
// FROM:
int write_result = buckets_batched_parallel_write_chunks(...);

// TO:
extern int buckets_parallel_write_chunks(...);
int write_result = buckets_parallel_write_chunks(...);
```

Rebuild and redeploy:
```bash
make clean && make
docker build -t russellmy/buckets:non-batched .
kubectl set image statefulset/buckets buckets=russellmy/buckets:non-batched -n buckets
```

## Future Enhancements

### 1. Dynamic Batch Sizing
Currently uses fixed grouping by node. Could optimize based on:
- Network bandwidth availability
- Node load
- Chunk size distribution

### 2. Batch Reads
Apply same optimization to GET operations:
- Batch read multiple chunks in single request
- Further reduce latency for large object retrieval

### 3. Pipelined Batching
Don't wait for all batches to complete before returning:
- Start sending batches as soon as first group is ready
- Overlap encoding with transmission
- Expected: Additional 10-20% improvement

### 4. Adaptive Batching
Switch between batched and non-batched based on:
- Object size (small objects may not benefit)
- Network conditions
- Cluster topology

## Metrics to Track

### Before/After Comparison

| Metric | Baseline | Target | Actual |
|--------|----------|--------|--------|
| Throughput (16w) | 211 ops/sec | 280 ops/sec | TBD |
| Latency (avg) | 75ms | 55ms | TBD |
| Latency (p99) | 264ms | 180ms | TBD |
| Network requests/object | 12 | 3 | TBD |
| Success rate | 100% | 100% | TBD |

### New Metrics

- **Batch efficiency**: Avg chunks per batch request
- **Grouping overhead**: Time spent grouping chunks by node
- **Batch transmission time**: Time to send batch vs individual chunks

## Code Statistics

**New Code**:
- `batch_transport.c`: ~450 lines
- `parallel_chunks_batched.c`: ~300 lines
- Header updates: ~60 lines
- **Total**: ~810 lines of new optimization code

**Modified Code**:
- `object.c`: 3 lines changed
- `binary_transport.c`: ~20 lines added (registration + exports)
- **Total**: ~23 lines modified

**Lines of Code Impact**: +810 new, ~23 modified

## Success Criteria

✅ **Build succeeds** - Compilation with no errors  
⏳ **Tests pass** - Unit and integration tests validate correctness  
⏳ **Performance improves** - 30-50% throughput increase measured  
⏳ **No regressions** - 100% success rate maintained  
⏳ **Cluster stable** - All nodes operational after deployment  

---

**Status**: Implementation complete, ready for testing and deployment.

**Next Steps**:
1. Deploy to test Kubernetes cluster
2. Run performance benchmarks
3. Compare metrics against baseline
4. Document results
5. Deploy to production if successful
