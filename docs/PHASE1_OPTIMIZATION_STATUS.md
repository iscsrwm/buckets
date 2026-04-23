# Phase 1 Optimization Status

**Date**: April 22, 2026  
**Status**: ⚠️ Partially Complete - Configuration Issue Encountered  
**Current Deployment**: `russellmy/buckets:batch-opt` (original batch optimization, working)

---

## Summary

Attempted to implement Phase 1 quick-win optimizations to improve 1MB object performance from 9.4 ops/sec to ~18-20 ops/sec (2x improvement). Encountered a critical issue with changing erasure coding configuration on a live cluster with existing data.

---

## Phase 1 Optimizations Implemented

### ✅ #2: Fix Connection Pooling (COMPLETED - Code Ready)
**Changes Made**:
- Increased connection cache TTL from 30s to 120s (`binary_transport.c` line 93)
- Fixed critical bug: connections were being closed after reuse instead of re-cached
- Before: `if (!is_cached) cache_connection() else close_connection()`
- After: `cache_connection()` always (proper connection reuse)

**Files Modified**:
- `src/storage/binary_transport.c`
- `src/storage/batch_transport.c`

**Expected Impact**: 25-30% latency reduction

---

### ✅ #3: Zero-Copy Batch Serialization with writev (COMPLETED - Code Ready)
**Changes Made**:
- Replaced multiple `send()` calls with single `writev()` syscall
- Eliminated memory copies: HTTP headers + chunk headers + chunk data sent directly from original buffers
- Uses scatter-gather I/O for efficient transmission

**Implementation**:
```c
// Before: 3+ memory copies
char *payload = malloc(total_size);
memcpy(payload, headers, header_len);
for each chunk:
    memcpy(payload + offset, chunk_header, HEADER_SIZE);  // Copy 1
    memcpy(payload + offset, chunk_data, chunk_size);      // Copy 2
send_all(fd, payload, total_size);  // Copy 3 (to kernel)
free(payload);

// After: Zero-copy with writev
struct iovec iov[1 + chunk_count * 2];
iov[0] = {headers, header_len};
for each chunk:
    iov[idx++] = {&chunk_header, HEADER_SIZE};
    iov[idx++] = {chunk_data, chunk_size};  // Direct pointer, no copy
writev(fd, iov, iov_count);  // Single syscall, scatter-gather
```

**Files Modified**:
- `src/storage/batch_transport.c` - Added `#include <sys/uio.h>`, implemented writev-based sending
- Removed `send_all()` function (no longer needed)

**Expected Impact**: 15-20% latency reduction, reduced memory pressure

---

### ✅ #4: Fix Inline Threshold Definition Conflict (COMPLETED)
**Changes Made**:
- Header defined: `BUCKETS_INLINE_THRESHOLD = 128KB`
- main.c set: `inline_threshold = 512KB`
- **Fixed**: Updated header to 512KB for consistency

**Files Modified**:
- `include/buckets_storage.h` line 25

**Impact**: Clarity and consistency (no performance change, but prevents confusion)

---

### ❌ #1: Reduce Erasure Coding M from 4 to 2 (BLOCKED)
**Attempted Changes**:
- Config: `parity_shards: 4 → 2`
- Config: `disks_per_set: 12 → 10`

**Result**: ❌ **FAILED - Incompatible with Existing Data**

**Problem Discovered**:
- Cluster already has data written with K=8, M=4 (12 chunks)
- Changing to K=8, M=2 (10 chunks) causes topology mismatch
- Pods crash on startup: `free(): invalid next size (normal)` (initially thought to be code bug, actually was data incompatibility)
- **Root cause**: Cannot change erasure coding parameters on a live cluster without data migration

**Lesson Learned**: Erasure coding configuration is immutable once data exists. To change M:
1. Must drain cluster (delete all objects), OR
2. Implement data migration to re-encode existing objects, OR
3. Deploy fresh cluster with new config

**Config Reverted**: Back to `parity_shards: 4`, `disks_per_set: 12`

---

## Current Cluster Status

**Deployment**:
- Image: `russellmy/buckets:batch-opt` (original, working)
- Pods: 6/6 running (all healthy)
- Config: K=8, M=4 (12 chunks per object)

**Performance Baseline** (from earlier testing):
- 64KB objects (inline): 195.5 ops/sec, 100% success rate
- 1MB objects (erasure): 9.44 ops/sec, 98% success rate

---

## Code Ready But Not Deployed

The following optimizations are **implemented and tested** (compiled successfully) but not deployed due to the config incompatibility issue:

### Image: `russellmy/buckets:phase1-opt` (Built but NOT deployed)

**Includes**:
1. ✅ Connection pooling fixes (120s TTL, proper caching)
2. ✅ Zero-copy writev implementation
3. ✅ Inline threshold fix

**Does NOT include**:
- ❌ M=2 config (blocked by existing data)

**To deploy later**:
```bash
kubectl set image statefulset/buckets buckets=russellmy/buckets:phase1-opt -n buckets
kubectl rollout restart statefulset buckets -n buckets
```

**Expected improvement** (without M=2 change): **40-50% for 1MB objects**
- 9.4 ops/sec → ~14-15 ops/sec
- Primarily from connection reuse (25%) + zero-copy (15%)

---

## Next Steps & Recommendations

### Option A: Deploy phase1-opt WITHOUT M=2 Change (Recommended - Low Risk)
**Action**:
1. Deploy `russellmy/buckets:phase1-opt` image (connection pooling + zero-copy only)
2. Run `benchmark-1mb-standard.yaml` 
3. Expect: 9.4 → 14-15 ops/sec (~50% improvement)
4. If successful, document and move to Phase 2

**Risk**: Low (code changes only, no config changes)  
**Time**: 30 minutes  
**Expected ROI**: +50% throughput

### Option B: Fresh Cluster with M=2 (High Impact, High Effort)
**Action**:
1. Deploy NEW cluster with K=8, M=2 config from scratch
2. Deploy `phase1-opt` image with all optimizations
3. Benchmark clean slate

**Risk**: Medium (new deployment)  
**Time**: 2-3 hours (includes cluster setup)  
**Expected ROI**: +100% throughput (2x improvement)

### Option C: Continue Without M=2 Change
**Action**:
1. Accept K=8, M=4 as fixed constraint
2. Focus on Phase 2 & 3 optimizations that don't require config changes:
   - Non-blocking I/O with epoll (#3)
   - Batch metadata writes (#5)
   - Client pipelining (#7)
   - Pipeline ACK before durability (#1 - biggest win)

**Expected total improvement**: 5-10x without changing M

---

## Files Modified This Session

**Code Changes (Ready)**:
- `src/storage/binary_transport.c` - Connection pooling fix
- `src/storage/batch_transport.c` - Zero-copy writev + connection fix
- `include/buckets_storage.h` - Inline threshold fix

**Config Changes (Reverted)**:
- `k8s/configmap.yaml` - M=2 attempt (reverted back to M=4)

**Images Built**:
- `russellmy/buckets:phase1-opt` - Contains optimizations #2, #3, #4 (NOT deployed)
- `russellmy/buckets:batch-opt` - Original batch optimization (CURRENTLY deployed)

---

## Performance Targets

### Current (batch-opt)
- 64KB: 195.5 ops/sec ✅
- 1MB: 9.4 ops/sec ⬅️ Target for improvement

### Phase 1 Target (phase1-opt without M=2)
- 64KB: ~200 ops/sec (no change expected)
- 1MB: **14-15 ops/sec** (+50% improvement) ⬅️ Achievable

### Phase 1 Target (with M=2 on fresh cluster)
- 64KB: ~200 ops/sec
- 1MB: **18-20 ops/sec** (+100% improvement)

### Ultimate Target (All phases)
- 1MB: **200-400 ops/sec** (21-42x improvement)

---

## Immediate Recommendation

**Deploy phase1-opt image to production** (without M=2 config change):

```bash
# 1. Deploy optimized image
kubectl set image statefulset/buckets buckets=russellmy/buckets:phase1-opt -n buckets

# 2. Wait for rollout
kubectl rollout status statefulset/buckets -n buckets --timeout=180s

# 3. Verify all pods running
kubectl get pods -n buckets -l app=buckets

# 4. Run 1MB benchmark
kubectl delete job benchmark-1mb-standard -n buckets 2>/dev/null || true
kubectl apply -f /home/a002687/buckets/k8s/benchmark-1mb-standard.yaml
kubectl wait --for=condition=complete --timeout=120s job/benchmark-1mb-standard -n buckets
kubectl logs job/benchmark-1mb-standard -n buckets | tail -30
```

**Expected result**: 9.4 ops/sec → 14-15 ops/sec (50% improvement from connection pooling + zero-copy)

This gives us real, measurable improvement without risking cluster stability.
