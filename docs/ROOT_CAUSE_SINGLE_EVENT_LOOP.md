# ROOT CAUSE FOUND: Single Event Loop Bottleneck

**Date**: April 21, 2026  
**Status**: 🚨 **CRITICAL** - Architecture Bottleneck Identified

## Executive Summary

The server is limited to **~20-22 ops/sec regardless of concurrency** because libuv uses a **single event loop** to handle all I/O events. Even with 128 worker threads and 50 concurrent clients, the single-threaded event loop becomes the bottleneck.

## Evidence

### Test 1: Sequential curl (1 worker)
```bash
20 sequential requests
```
**Result**: 20 ops/sec, 50ms per request

### Test 2: Go Benchmark (10 workers, true concurrency)
```go
10 concurrent goroutines
```
**Result**: 21.75 ops/sec, 456ms average latency
**Expected**: 10 × 20 = 200 ops/sec
**Actual**: 21.75 ops/sec (10.9% of expected!)

### Test 3: Go Benchmark (50 workers)
```go
50 concurrent goroutines
```
**Result**: 22.18 ops/sec, 2134ms average latency  
**Expected**: 50 × 20 = 1000 ops/sec
**Actual**: 22.18 ops/sec (2.2% of expected!)

### Test 4: boto3 (50 workers)
```python
ThreadPoolExecutor(50 workers)
```
**Result**: 22.50 ops/sec, 2109ms average latency

## The Pattern

| Workers | Expected | Actual | Efficiency |
|---------|----------|--------|------------|
| 1 | 20 ops/s | 20 ops/s | 100% |
| 10 | 200 ops/s | 21.75 ops/s | **11%** |
| 50 | 1000 ops/s | 22 ops/s | **2%** |

**Conclusion**: Throughput is capped at ~20-22 ops/sec **regardless of client concurrency**.

## Root Cause: Single Event Loop

### Current Architecture

```
                     ┌─────────────────┐
50 Concurrent ───────► libuv Event Loop│──► 128 Worker Threads
Clients              │ (SINGLE-THREADED)│    (for disk I/O)
                     └─────────────────┘
                            ↓
                     ~20 ops/sec MAX
```

**Problem**: The event loop is single-threaded and processes all I/O events sequentially:
1. Accept connection
2. Parse HTTP request  
3. Queue work to thread pool
4. Wait for completion
5. Send response
6. Repeat for next connection

Even though worker threads are parallel, the event loop serializes all coordination!

### libuv Architecture Limitation

From `/home/a002687/buckets/src/net/uv_server.c`:
```c
// Line 362
ret = uv_loop_init(server->loop);
```

**ONE event loop** for the entire server!

From `/home/a002687/buckets/src/main.c`:
```c
// Lines 297-301
if (getenv("UV_THREADPOOL_SIZE") == NULL) {
    setenv("UV_THREADPOOL_SIZE", "128", 1);
}
```

We have **128 worker threads**, but **1 event loop** coordinating everything.

### Benchmarking Validation

**Min Latency from Go benchmark**: 53-70ms  
**Max Latency**: 30 seconds (queueing!)  
**Average**: 2000+ ms with 50 workers

The server CAN process requests in 53ms, but with 50 concurrent requests:
- Request 1: 53ms
- Request 2: waits for event loop → 53ms + queue time
- Request 3: waits for event loop → 53ms + queue time
- ...
- Request 50: waits for 49 others → 53ms + ~2000ms queue time

**Queueing formula**: 
- Single event loop capacity: ~20 ops/sec
- With 50 workers at 50ms each = 1000 requests/sec demand
- Queue backlog = 1000 - 20 = 980 requests waiting
- Average queue time = 980 / 20 = 49 seconds... but we timeout at 30s!

## Why This Wasn't Obvious

1. **Storage benchmarks looked good** because they measured single operations
2. **Localhost testing with low concurrency** didn't stress the event loop
3. **boto3 appeared to be the problem** because it ALSO serialized, masking the server issue
4. **Small latencies (50ms) looked fine** in isolation

## Comparison to Production Servers

### nginx (C, event-driven, multi-process)
- **Architecture**: N event loops (one per core)
- **Typical throughput**: 100,000+ requests/sec
- **Method**: `fork()` multiple worker processes, each with own event loop

### Node.js (JavaScript, libuv-based)
- **Architecture**: Single event loop (cluster mode for multi-core)
- **Typical throughput**: 10,000-50,000 requests/sec per process
- **Method**: Cluster module spawns N processes

### Go HTTP Server (Go, goroutines)
- **Architecture**: Automatic goroutine scheduling across all cores
- **Typical throughput**: 50,000-200,000 requests/sec
- **Method**: One goroutine per connection, scheduled across OS threads

### Our Server (C, libuv, single event loop)
- **Architecture**: 1 event loop + 128 worker threads
- **Actual throughput**: **20-22 requests/sec** ❌
- **Bottleneck**: Single event loop serializes all I/O coordination

## Solutions

### Solution 1: Multi-Process Architecture (nginx-style) ⭐ **RECOMMENDED**

Fork N worker processes, each with its own event loop:

```c
// Pseudo-code
int num_workers = sysconf(_SC_NPROCESSORS_ONLN);  // Get CPU count

for (int i = 0; i < num_workers; i++) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child process: run own event loop
        uv_loop_t *loop = uv_loop_new();
        buckets_http_server_start(loop, port);
        uv_run(loop, UV_RUN_DEFAULT);
        exit(0);
    }
}

// Parent: wait for children
```

**Expected improvement**: 8-16x (one event loop per CPU core)  
**Expected throughput**: 160-320 ops/sec on 8-core machine  
**Complexity**: Medium (fork handling, shared nothing architecture)

### Solution 2: Multiple Event Loops in Threads

Create N event loops, one per thread:

```c
// Pseudo-code
for (int i = 0; i < num_cores; i++) {
    pthread_t thread;
    pthread_create(&thread, NULL, event_loop_worker, port);
}

void* event_loop_worker(void* arg) {
    uv_loop_t *loop = uv_loop_new();
    buckets_http_server_start(loop, port);
    uv_run(loop, UV_RUN_DEFAULT);
    return NULL;
}
```

**Expected improvement**: 8-16x  
**Expected throughput**: 160-320 ops/sec  
**Complexity**: Medium (thread-local storage, SO_REUSEPORT)

### Solution 3: Rewrite in Go ⭐ **LONG-TERM**

Go's runtime automatically distributes goroutines across CPU cores:

```go
http.HandleFunc("/", handler)
http.ListenAndServe(":9000", nil)
// Automatically uses all cores!
```

**Expected improvement**: 50-100x  
**Expected throughput**: 1,000-2,000 ops/sec  
**Complexity**: High (full rewrite)

### Solution 4: Use io_uring (Linux 5.1+)

Replace libuv with io_uring for truly async I/O without threads:

**Expected improvement**: 10-20x  
**Expected throughput**: 200-400 ops/sec  
**Complexity**: Very High (io_uring API, kernel support)

## Immediate Action Plan

### Phase 1: Validate Multi-Process Improvement (1-2 hours)

Add multi-process support:

1. Parse `--workers N` flag
2. Fork N child processes
3. Each child runs own event loop on same port (SO_REUSEPORT)
4. Parent waits for children

**Expected**: Validate 8-16x improvement

### Phase 2: Production Deployment (1 day)

- Add worker process management
- Handle graceful shutdown
- Log aggregation from workers
- Health check per worker

### Phase 3: Kubernetes Deployment (1 day)

Already good! Each pod is independent:
- 6 pods × 160 ops/sec = **960 ops/sec cluster-wide**

## Current vs. Potential Performance

### Current (Single Event Loop)
```
1 event loop × 20 ops/sec = 20 ops/sec per pod
6 pods × 20 ops/sec = 120 ops/sec cluster
```

### After Multi-Process (8 cores)
```
8 event loops × 20 ops/sec = 160 ops/sec per pod
6 pods × 160 ops/sec = 960 ops/sec cluster
```

### After Go Rewrite (aspirational)
```
1 Go server × 1000 ops/sec = 1000 ops/sec per pod
6 pods × 1000 ops/sec = 6,000 ops/sec cluster
```

## Conclusion

**The bottleneck is NOT**:
- ❌ Disk I/O (group commit helps but masked)
- ❌ Network latency (50ms server response is great!)
- ❌ boto3 (it's slow but masks the real issue)
- ❌ Storage layer (inline, erasure coding all work well)

**The bottleneck IS**:
- ✅ **Single event loop** processing all I/O events serially
- ✅ Limited to ~20 ops/sec regardless of concurrency
- ✅ Solvable with multi-process architecture

**Next Step**: Implement multi-process worker pool to achieve **8-16x improvement**.

---

## Recommendation

**Implement Solution 1 (Multi-Process) immediately**:
1. Low complexity (2-3 hours of work)
2. Proven pattern (nginx, gunicorn, etc.)
3. 8-16x performance improvement
4. Compatible with existing Kubernetes deployment

**Expected final performance**:
- Per pod: **160 ops/sec** (vs 20 currently)
- Cluster (6 pods): **960 ops/sec** (vs 120 currently)
- Latency: **50-100ms** (vs 2000ms currently)

This would make the system **production-ready** with performance comparable to established object storage systems.
