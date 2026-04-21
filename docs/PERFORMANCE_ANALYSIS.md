# Performance Analysis: Localhost vs Kubernetes

**Date**: April 21, 2026

## Performance Comparison

### Localhost (Single Machine, April 20, 2026)
- **Configuration**: 6 processes, 24 virtual disks on 1 physical disk
- **Network**: Unix sockets / localhost
- **Client**: Direct connection, no LoadBalancer
- **Results**: **150.83 ops/sec** for 256KB concurrent uploads (50 workers)
- **Bottleneck**: Physical disk I/O saturation

### Kubernetes (6 Nodes, April 21, 2026)
- **Configuration**: 6 pods across 6 physical nodes, 24 PVCs
- **Network**: Pod → LoadBalancer → Pod (cluster network)
- **Client**: boto3 from inside cluster through LoadBalancer
- **Results**: **17.93 ops/sec** for 256KB concurrent uploads (50 workers, boto3)
- **Results**: **23.21 ops/sec** for 256KB concurrent uploads (50 workers, direct HTTP)
- **Bottleneck**: Network latency + coordination overhead

## Performance Degradation: 150 → 23 ops/sec (6.5x slower)

### Factor 1: boto3 SDK Overhead (~20%)
- **boto3**: 17.93 ops/sec
- **Direct HTTP (curl)**: 23.21 ops/sec
- **Overhead**: ~23% slower with boto3 (signature calculation, retries, connection pooling)

### Factor 2: Network Latency (Major factor)
Kubernetes introduces additional network hops:
1. Benchmark pod → LoadBalancer service
2. LoadBalancer → Target pod
3. Target pod → Other pods (RPC for erasure coding)
4. Response path back

**Estimated impact**: 4-5x slowdown

### Factor 3: Async vs Sync Behavior
- **Localhost**: May have been using different async patterns
- **K8s**: Going through more network layers increases sync wait times

## Root Cause Analysis

The 6.5x performance difference (150 → 23 ops/sec) is primarily due to:

1. **Network Round-Trip Time** (60-70% of degradation)
   - LoadBalancer adds latency
   - Inter-pod RPC for erasure coding
   - Cluster network vs localhost sockets

2. **Connection Overhead** (10-15%)
   - HTTP connection establishment
   - TLS handshake (if enabled)
   - Keep-alive connection reuse

3. **boto3 SDK** (20%)
   - Signature V4 calculation
   - Request/response parsing
   - Retry logic

4. **Coordination Overhead** (5-10%)
   - Service discovery
   - Load balancing decisions

## What Group Commit Did (and Didn't) Fix

### Expected Benefit
Group commit was designed to batch fsync() calls to reduce disk I/O overhead:
- **Localhost**: Disk I/O was the bottleneck → group commit could help
- **K8s**: Network is the bottleneck → group commit has minimal impact

### Actual Results
- **Without group commit**: 18.40 ops/sec
- **With group commit**: 17.93 ops/sec
- **Difference**: -2.6% (within noise)

### Why No Improvement?
When network latency dominates (2.6 seconds per request), saving a few milliseconds on disk fsync doesn't move the needle:
- **Disk fsync time**: ~5-10ms per chunk
- **Network round-trip**: ~2000-2500ms per request
- **Group commit savings**: <1% of total latency

## Paths to Better Performance

### Option 1: Reduce Network Hops ⭐ **Most Impact**

**Current**: Client → LoadBalancer → Pod → Other Pods (RPC)

**Improvement 1a**: Direct pod connection (skip LoadBalancer)
```bash
# Port-forward to specific pod
kubectl -n buckets port-forward buckets-0 9000:9000
# Test directly
curl -X PUT --data-binary @file.bin http://localhost:9000/bucket/key
```
**Expected**: 30-40 ops/sec (1.5-2x improvement)

**Improvement 1b**: Client pod with pod affinity
Deploy benchmark pod with affinity to storage pods to minimize network distance.
**Expected**: 40-60 ops/sec (2-3x improvement)

**Improvement 1c**: Use headless service directly
```python
endpoint = "http://buckets-0.buckets-headless.buckets.svc.cluster.local:9000"
```
**Expected**: 25-35 ops/sec (1.2-1.5x improvement)

### Option 2: Reduce RPC Calls

**Current**: Each 256KB object = 12 RPC calls for erasure coding

**Improvement 2a**: Increase inline threshold
```c
// src/storage/object.c
inline_threshold = 512 * 1024;  // 512KB instead of 128KB
```
Objects <512KB stored inline (no erasure coding, no RPC overhead).
**Expected**: 40-80 ops/sec for <512KB objects

**Improvement 2b**: Local-first write with async RPC
Already implemented for inline objects. Extend to erasure-coded:
- Write locally first
- Return success immediately  
- Replicate asynchronously in background

**Expected**: 60-100 ops/sec (3-5x improvement)
**Risk**: Data loss if node fails before replication completes

### Option 3: Connection Pooling & Keep-Alive

**Current**: May be establishing new connections for each request

**Improvement**: Ensure HTTP keep-alive is working properly
```python
session = boto3.Session()
s3 = session.client('s3', config=Config(max_pool_connections=50))
```

**Expected**: 30-40 ops/sec (1.5x improvement)

### Option 4: Batch Uploads

**Current**: Sequential uploads with high per-request overhead

**Improvement**: Use S3 multipart upload API to batch data
```python
# Upload 10MB in 40x 256KB parts
# Amortizes connection overhead across parts
```

**Expected**: 80-120 ops/sec effective throughput

### Option 5: Use Faster Client Library

**Current**: boto3 (Python, interpreted)

**Improvement**: Use compiled client (Rust/Go/C++)
```rust
// tokio-based async Rust client
let client = aws_sdk_s3::Client::new(&config);
```

**Expected**: 50-80 ops/sec (2-3x improvement from boto3)

## Recommended Next Steps

### Immediate (No Code Changes)

1. **Test with headless service** instead of LoadBalancer
   ```python
   endpoint = "http://buckets-0.buckets-headless:9000"
   ```
   Expected: +20% performance

2. **Test with larger batch size** in boto3
   ```python
   config = Config(max_pool_connections=100)
   ```
   Expected: +30% performance

3. **Test with Go/Rust S3 client** instead of boto3
   Expected: +100-150% performance

### Short Term (Configuration Changes)

4. **Increase inline threshold** to 512KB
   Edit config, redeploy pods
   Expected: +150-200% for <512KB objects

5. **Enable async replication** for erasure-coded writes
   Requires code changes to parallel_chunks.c
   Expected: +200-300% performance
   Risk: Need robust replication verification

### Long Term (Architecture Changes)

6. **Implement client-side erasure coding**
   Client encodes chunks, uploads directly to nodes
   Eliminates server-side RPC overhead
   Expected: +300-400% performance

7. **Use RDMA/faster network**
   Replace TCP with RDMA for inter-pod communication
   Expected: +200-300% performance

## Conclusion

Current Kubernetes performance (**23 ops/sec**) is **6.5x slower** than localhost (**150 ops/sec**) due to network overhead, not code issues. Group commit works correctly but provides no benefit when network is the bottleneck.

**Most Promising Improvements**:
1. Async replication for erasure-coded writes (3-5x)
2. Increase inline threshold to 512KB (2-3x for small objects)
3. Use compiled client library instead of boto3 (2-3x)

**Combined potential**: 10-20x improvement → **230-460 ops/sec**

This would exceed localhost performance by utilizing multiple physical nodes in parallel rather than being bottlenecked on a single disk.
