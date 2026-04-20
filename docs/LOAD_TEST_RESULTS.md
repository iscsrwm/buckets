# Buckets 6-Node Cluster Load Test Results

**Date**: March 6, 2026  
**Version**: 0.1.0-alpha  
**Test Environment**: localhost (6 nodes on ports 9001-9006)

## Cluster Configuration

- **Nodes**: 6
- **Disks per node**: 4
- **Total disks**: 24
- **Erasure sets**: 2 (12 disks per set)
- **Erasure coding**: K=8, M=4 (8 data shards, 4 parity shards)
- **Deployment ID**: `6node-unified-cluster`

## Test Tools

- **boto3**: AWS SDK for Python with S3v4 signatures
- **Test files**: 1KB, 64KB, 256KB, 1MB random binary data

---

## Current Results (March 6, 2026)

> **Note**: Previous results from March 3, 2026 were invalid due to a missing `Expect: 100-continue` 
> response which caused boto3/AWS SDK clients to wait 1 second before sending request bodies.
> This has been fixed and the results below reflect actual performance.

### Concurrent Workload Performance (50 workers, 20 seconds)

| Workload | Total Ops | Throughput | Avg Rate | Peak Rate |
|----------|-----------|------------|----------|-----------|
| **GET-only** | 10,216 | 394 ops/s | 410/s | 1,640/s |
| **PUT-only** | 7,945 | 383 ops/s | 404/s | 980/s |
| **Mixed (50/50)** | 9,411 | 446 ops/s | 451/s | 700/s |

### Performance by Object Size (Sequential, Single Client)

| Object Size | PUT ops/s | GET ops/s |
|-------------|-----------|-----------|
| **1KB** | 140 | 563 |
| **64KB** | 101 | 503 |
| **256KB** | 13 | 162 |
| **1MB** | 11 | 68 |

### Single Operation Latency (Authenticated)

| Operation | Latency |
|-----------|---------|
| PUT 1KB | 7-10ms |
| GET 1KB | 2-3ms |
| HEAD | 1-2ms |
| DELETE | 10-15ms |

---

## Bug Fix: HTTP 100-Continue Support

**Issue**: Prior to March 6, 2026, the server did not respond to `Expect: 100-continue` headers.
This caused AWS SDKs (boto3, AWS CLI, mc client) to wait 1 second before sending request bodies.

**Impact**: PUT operations took ~1000ms instead of ~10ms.

**Fix**: Server now responds with `HTTP/1.1 100 Continue` when clients send `Expect: 100-continue`.

| Metric | Before Fix | After Fix | Improvement |
|--------|------------|-----------|-------------|
| Single PUT latency | ~1000ms | ~7ms | **143x faster** |
| PUT throughput | ~1 ops/s | ~140 ops/s | **140x faster** |

---

## Data Integrity Verification

All operations verified with MD5 checksums using authenticated requests:

| Object Size | PUT | GET | MD5 Match |
|-------------|-----|-----|-----------|
| 1KB | OK | OK | **PASS** |
| 64KB | OK | OK | **PASS** |
| 256KB | OK | OK | **PASS** |
| 1MB | OK | OK | **PASS** |

### Verification Method

1. Generate random binary data with `dd if=/dev/urandom`
2. Compute MD5 hash of original data
3. PUT object to cluster via authenticated S3 API
4. GET object from cluster via authenticated S3 API
5. Compute MD5 hash of retrieved data
6. Compare hashes - must match exactly

---

## Key Findings

1. **PUT performance restored**: 140 ops/s single-client, 383 ops/s concurrent (was ~1 ops/s)
2. **GET performance strong**: 563 ops/s single-client, 394 ops/s concurrent
3. **Mixed workloads efficient**: 446 ops/s with 50% GET / 50% PUT mix
4. **Low latency**: 7-10ms PUT, 2-3ms GET for 1KB objects
5. **Data integrity verified**: MD5 checksums match for all object sizes
6. **Zero failures** in all authenticated tests

## Performance Characteristics

- **Small objects (1KB-64KB)**: Best throughput, inline storage path
- **Large objects (256KB+)**: Erasure coding overhead reduces throughput
- **GET faster than PUT**: Reads benefit from parallel chunk retrieval
- **Authentication overhead minimal**: ~1-2ms per request

---

## Test Commands Reference

```bash
# Start 6-node cluster
for i in 1 2 3 4 5 6; do
  ./bin/buckets server --config config/6node-unified-node$i.json &
done

# Configure mc client
mc alias set buckets http://localhost:9001 minioadmin minioadmin

# PUT test with mc
mc cp test-file.bin buckets/test-bucket/

# Python benchmark
python3 scripts/full_benchmark.py
```

---

## Historical Results (March 3, 2026) - INVALID

> **WARNING**: The results below were measured before the `100-continue` fix and represent
> artificially high numbers due to unsigned/unauthenticated requests bypassing the streaming
> upload path. Real-world S3 clients (boto3, AWS SDKs, mc) use `Expect: 100-continue` and
> would have experienced ~1 ops/s PUT performance until the March 6, 2026 fix.

The following historical data is preserved for reference but should not be used for comparison:

<details>
<summary>Click to expand invalid historical results</summary>

### Single Node Performance (Unsigned Requests Only)

| Object Size | PUT (req/s) | GET (req/s) |
|-------------|-------------|-------------|
| 1KB | 8,630 | 9,861 |
| 64KB | 5,582 | 4,167 |
| 1MB | 1,648 | 388 |

### Cluster Performance (Unsigned Requests Only)

| Test | Total Throughput | Connections |
|------|------------------|-------------|
| Parallel | 37,157 req/s | 150 |
| High Concurrency | 37,430 req/s | 600 |

These numbers were achieved using Apache Benchmark (`ab`) which does not use AWS authentication
or `Expect: 100-continue`, thus bypassing the performance issue that affected real S3 clients.

</details>
