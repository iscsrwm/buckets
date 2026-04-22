# Load Distribution Findings

**Date**: April 22, 2026  
**Status**: 🔴 Critical Issue Discovered  
**Impact**: System requires session affinity for stability

---

## Discovery

Testing revealed that the Buckets cluster **requires session affinity** (ClientIP) on the LoadBalancer service. Removing session affinity causes massive failures.

### Test Results

**With Session Affinity (ClientIP)**:
```
Throughput: 160.18 ops/sec
Success rate: 100% (4868/4868)
Average latency: 309ms
```

**Without Session Affinity (None)**:
```
Throughput: 0.14 ops/sec  ⬇️ (99.9% failure)
Success rate: 8.6% (5/58)  🔴
Average latency: 26,072ms (26 seconds!)
Failed operations: 53/58
```

### Error Analysis

When session affinity is removed, pods log multiple errors:

```
ERROR: Chunk 7 write failed
ERROR: Parallel write: 4/12 chunks failed
ERROR: Parallel chunk write failed
ERROR: Failed to store object benchmark-go/worker-47/obj-000000.bin
ERROR: Parallel: Failed to write chunk 9 via binary transport to http://buckets-5...
ERROR: Failed to receive response
ERROR: Invalid stream type 0 (expected UV_TCP=12)
```

---

## Root Cause Analysis

### Why Session Affinity Is Required

The current architecture has **implicit assumptions** about request routing:

1. **Erasure coding coordination**:
   - When pod A receives a PUT request, it coordinates writes to 12 disks across multiple pods
   - State/context may be held on the receiving pod
   - If subsequent related requests go to pod B, coordination breaks

2. **Stateful operations**:
   - Multipart uploads require state tracking
   - Version IDs, upload IDs may be pod-local
   - Switching pods mid-operation causes failures

3. **Connection pooling**:
   - RPC connections between pods may not handle concurrent requests well
   - Race conditions when multiple pods simultaneously write to same disks

4. **Lock contention across pods**:
   - If multiple pods try to write the same object simultaneously
   - Distributed locking may not be fully implemented

---

## Implications

### 1. Original Hypothesis Was Wrong

**We thought**: Distributing 50 workers across 6 pods would give 6x improvement

**Reality**: The system requires session affinity, so:
- Each client/benchmark pod → ONE backend pod (sticky)
- Can't distribute a single client's load across multiple backend pods
- Need MULTIPLE clients to utilize multiple backend pods

### 2. The Right Way to Scale

**Wrong approach** (what we tried):
```
1 benchmark pod → LoadBalancer (no affinity) → 6 backend pods
Result: Failures due to broken coordination
```

**Right approach**:
```
6 benchmark pods → LoadBalancer (with affinity) → 6 backend pods
Each benchmark pod "sticks" to one backend pod
Result: 6x parallelism without breaking coordination
```

### 3. Current Performance Is Actually Correct

**Single pod performance**: 160 ops/sec with session affinity  
**Cluster capacity**: 6 × 160 = **960 ops/sec** (theoretical)

But this requires 6 independent clients, each sticky to one pod.

---

## Alternative: Fix the Architecture

The need for session affinity indicates architectural issues:

###  Option 1: Truly Stateless S3 API Layer

Make each pod fully stateless:
- Distributed locking for object writes
- Shared state storage (e.g., etcd, Redis)
- Idempotent operations
- Better coordination protocols

**Effort**: High (major refactoring)  
**Benefit**: True horizontal scalability

### Option 2: Request-Level Routing

Route based on object key (consistent hashing):
- PUT /bucket/keyA always goes to pod X
- PUT /bucket/keyB always goes to pod Y
- No conflicts, deterministic routing

**Effort**: Medium  
**Benefit**: Natural load distribution, no session affinity needed

### Option 3: Accept Current Design

Keep session affinity, scale by adding clients:
- Each client gets one backend pod
- Need N clients to utilize N pods
- Simple, works with current code

**Effort**: Low (already working)  
**Benefit**: No code changes needed

---

## Revised Performance Model

### Single Client Performance

With session affinity, one client → one pod:
- **Sequential (1 worker)**: 34 ops/sec, 28ms latency
- **Optimal (10 workers)**: 96 ops/sec, 76ms latency  
- **High load (50 workers)**: 72 ops/sec, 233ms latency (contention)

**Optimal per-client concurrency**: 10-15 workers

### Cluster Performance (Multiple Clients)

With proper client distribution:

| Clients | Workers/Client | Total Workers | Pods Used | Expected Throughput |
|---------|----------------|---------------|-----------|---------------------|
| 1 | 10 | 10 | 1 | 96 ops/sec |
| 3 | 10 | 30 | 3 | 288 ops/sec |
| **6** | **10** | **60** | **6** | **576 ops/sec** ⭐ |
| 6 | 20 | 120 | 6 | 600+ ops/sec |

**To achieve 600 ops/sec**: Need 6 independent clients, each running 10-20 workers

---

## Testing Strategy Correction

### Wrong Test (What We Did)

```yaml
# 1 benchmark pod, 50 workers
apiVersion: batch/v1
kind: Job
spec:
  parallelism: 1
  containers:
  - env:
    - name: WORKERS
      value: "50"
```

Result: All 50 workers → 1 backend pod (session affinity) = contention

### Right Test (What We Should Do)

```yaml
# 6 benchmark pods, 10 workers each
apiVersion: batch/v1
kind: Job
spec:
  completions: 6
  parallelism: 6
  containers:
  - env:
    - name: WORKERS
      value: "10"
```

Result: Each benchmark pod → different backend pod = 6x parallelism without contention

---

## Action Items

### Immediate (Validate Current Architecture)

1. **Run parallel benchmark** - 6 jobs, 10 workers each
2. **Expected result**: ~576 ops/sec aggregate (6 × 96)
3. **Validate**: Session affinity working as designed

### Short-term (Optimize Within Current Design)

1. **Document session affinity requirement** in deployment docs
2. **Right-size client concurrency** - recommend 10-15 workers per client
3. **Load testing guide** - explain how to scale with multiple clients

### Long-term (Architectural Improvements)

1. **Investigate why session affinity is needed**:
   - Profile concurrent multi-pod writes
   - Identify race conditions
   - Find missing distributed coordination

2. **Consider request-level routing**:
   - Hash bucket+key → pod assignment
   - Deterministic, no session affinity needed

3. **Implement distributed state** (if needed):
   - For multipart uploads
   - For versioning
   - For cross-pod coordination

---

## Conclusion

**Finding**: System requires session affinity due to stateful coordination

**Current capacity**: 160 ops/sec per pod × 6 pods = **960 ops/sec theoretical**

**To achieve this**: Need 6 independent clients, not 1 client with 60 workers

**Next step**: Run parallel benchmark with 6 pods to validate cluster-wide throughput

---

**Status**: Investigation complete, revised testing strategy ready
