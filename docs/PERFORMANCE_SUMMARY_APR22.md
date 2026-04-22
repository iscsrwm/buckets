# Performance Investigation Summary - April 22, 2026

**Date**: April 22, 2026  
**Status**: 🔴 Critical Issues Discovered  
**Outcome**: Multiple findings requiring further investigation

---

## Executive Summary

Today's performance optimization work uncovered several critical findings:

1. ✅ **Network is fast** (0.36ms RTT, not 300ms as assumed)
2. ✅ **Single-operation latency is low** (~30ms actual processing)
3. ✅ **Socket buffers were not the bottleneck** (optimization had no impact)
4. 🔴 **System fails under multi-client load** (critical issue discovered)
5. 🔴 **Session affinity is required but may not be sufficient**

---

## What We Did

### 1. Socket Buffer Optimization
- Implemented SO_SNDBUF/SO_RCVBUF = 256KB
- Deployed to Kubernetes cluster
- Result: **No performance change** (-2.1%, within variance)

### 2. Network RTT Measurement
- ICMP ping: **0.36ms average**
- TCP connect: ~1ms
- HTTP request: ~3-4ms
- **Finding**: Network is extremely fast, NOT 300ms

### 3. Concurrency Testing (Single Pod)
- Tested 1, 5, 10, 20, 50 workers hitting one pod
- **Finding**: Optimal = 10-20 workers per pod
- 50 workers causes contention (throughput drops to 72 ops/sec)

### 4. Load Distribution Testing
- Tested LoadBalancer with/without session affinity
- **Finding**: System requires session affinity or it fails
- Tested 3 and 6 parallel benchmark pods
- **Critical Issue**: Massive failures even with session affinity

---

## Key Findings

### Finding 1: Network Is Not The Bottleneck ✅

**Initial Assumption**: 300ms benchmark latency = network RTT  
**Reality**: Network RTT is 0.36ms

**Latency Breakdown** (50 workers → 1 pod):
- Network: ~0.4ms (<1%)
- Request processing: ~30ms (10%)
- **Queueing time: ~270ms** (90%) ← Real bottleneck

**Conclusion**: The 300ms is queueing time from concurrency contention, not network delay.

### Finding 2: Concurrency Contention Is The Bottleneck ✅

| Workers/Pod | Throughput | Latency | Status |
|-------------|------------|---------|--------|
| 1 | 34.52 ops/sec | 28.82ms | ✅ Baseline |
| 10 | 96.12 ops/sec | 76.38ms | ✅ **Optimal** |
| 20 | 102.24 ops/sec | 132.63ms | ⚠️ Some contention |
| 50 | 72.02 ops/sec | 233.21ms | 🔴 Severe contention |

**Conclusion**: Per-pod concurrency limit is ~10-20 workers. Beyond this, contention reduces throughput.

### Finding 3: Session Affinity Required 🔴

**With SessionAffinity=ClientIP**:
- Throughput: 160 ops/sec
- Success rate: 100%

**With SessionAffinity=None**:
- Throughput: 0.14 ops/sec
- Success rate: 8.6% (massive failures)
- Errors: "Chunk write failed", "Invalid stream type"

**Conclusion**: The system has stateful coordination that breaks without session affinity.

###  Finding 4: Multi-Client Failures 🔴 **CRITICAL**

**Test**: 3 parallel benchmark pods (30 total workers, session affinity enabled)

**Results**:
- Pod 1: 0.00 ops/sec (0/11 success)
- Pod 2: 0.26 ops/sec (9/20 success, 45% failure)
- Pod 3: 0.33 ops/sec (12/23 success, 48% failure)

**Test**: 6 parallel benchmark pods (60 total workers, session affinity enabled)

**Results**: 
- All pods: 0.12-0.50 ops/sec
- Failure rates: 37-68%
- Latencies: 10-21 seconds (extreme)

**Conclusion**: The system cannot handle concurrent load from multiple independent clients, even WITH session affinity. This is a critical production blocker.

---

## Root Cause Analysis

### Why Multi-Client Load Fails

The failures occur even with session affinity, suggesting deeper issues:

1. **Resource exhaustion**:
   - File descriptors
   - Worker thread pool saturation
   - Memory pressure

2. **Distributed coordination failures**:
   - Multiple clients writing to same backend storage
   - Race conditions in erasure-coded chunk writes
   - Lock contention across pods

3. **RPC connection pool issues**:
   - Pods communicate via RPC for chunk writes
   - Connection pools may be exhausted under load
   - Timeouts (30s) suggest blocking/deadlock

4. **Cascading failures**:
   - One slow operation blocks others
   - Thread pool exhaustion
   - Requests pile up, causing timeouts

---

## Performance Characteristics

### Single Client (Works Well) ✅

**Configuration**: 1 benchmark pod, 10-20 workers, session affinity  
**Performance**: 96-102 ops/sec, ~100ms latency  
**Status**: ✅ Production-ready for single-client workloads

### Multiple Clients (Fails) 🔴

**Configuration**: 3-6 benchmark pods, 10 workers each, session affinity  
**Performance**: 0.12-0.50 ops/sec, 37-68% failure rate  
**Status**: 🔴 NOT production-ready for multi-client workloads

---

## Corrected Performance Model

### What We Thought
- Cluster capacity: 6 pods × 160 ops/sec = 960 ops/sec
- Bottleneck: Network latency or socket buffers
- Solution: Optimize network stack

### What's Actually True
- **Single-client capacity**: ~100 ops/sec (works well)
- **Multi-client capacity**: UNKNOWN (currently fails)
- **Bottleneck**: Unknown systemic issue causing multi-client failures
- **Solution**: Debug why multiple clients cause failures

---

## Critical Issues Requiring Investigation

### Issue 1: Multi-Client Coordination Failures

**Symptoms**:
- 30-second timeouts
- Chunk write failures
- 37-68% failure rates

**Possible Causes**:
- Distributed lock contention
- RPC connection exhaustion
- Thread pool saturation
- Deadlocks in erasure coding coordination

**Next Steps**:
1. Enable debug logging
2. Profile lock contention
3. Monitor resource usage (file descriptors, threads, memory)
4. Check RPC connection pool stats

### Issue 2: Why Session Affinity Is Required

**Question**: What stateful coordination requires session affinity?

**Investigation Needed**:
- Review erasure coding coordination logic
- Check for pod-local state
- Identify race conditions when requests switch pods
- Determine if this is by design or a bug

### Issue 3: Resource Limits Under Load

**Question**: Are there hard limits being hit?

**Check**:
- Worker thread pool size (currently 16 × 64 = 1024 threads)
- File descriptor limits
- Memory usage under multi-client load
- RPC connection pool size (MAX_CONCURRENT_RPC_CALLS = 512)

---

## What Actually Works

### Proven Configurations ✅

1. **Single benchmark pod, 10-20 workers**:
   - Throughput: 96-102 ops/sec
   - Latency: 76-132ms
   - Success rate: 100%
   - **Status**: Reliable, production-ready

2. **Single pod directly accessed**:
   - Throughput: 160 ops/sec
   - Latency: 307ms
   - Success rate: 100%
   - **Status**: Works, but higher latency due to 50 workers

### Broken Configurations 🔴

1. **LoadBalancer without session affinity**:
   - Massive failures (92% failure rate)
   - Not viable

2. **Multiple concurrent clients** (3-6 pods):
   - 37-68% failure rate
   - Extreme latencies (10-20 seconds)
   - Not production-ready

---

## Recommendations

### Immediate Actions

1. **Do NOT remove session affinity** - System breaks without it

2. **Do NOT deploy for multi-client workloads** - Current system cannot handle it

3. **Document single-client limitation** - Current production capacity is ~100 ops/sec from one client

4. **Investigate multi-client failures** - This is a critical blocker

### Investigation Priorities

1. **Enable comprehensive logging**:
   - RPC failures
   - Lock contention
   - Thread pool status
   - Resource usage

2. **Profile under multi-client load**:
   - Where are the 30-second timeouts?
   - What's blocking?
   - Resource exhaustion?

3. **Review distributed coordination**:
   - Erasure coding write path
   - Cross-pod RPC patterns
   - Potential deadlocks

### Long-Term Solutions

1. **Fix multi-client support** (critical):
   - Debug and resolve coordination failures
   - Increase resource limits if needed
   - Improve error handling and timeouts

2. **Make system truly stateless** (if possible):
   - Eliminate need for session affinity
   - Enable request-level load balancing
   - Better horizontal scaling

3. **Increase per-pod concurrency limit**:
   - Currently ~10-20 workers optimal
   - Target: 30-50 workers without contention
   - Requires reducing lock contention

---

## Lessons Learned

1. **Always measure, never assume**:
   - We assumed 300ms = network RTT
   - Actual network: 0.36ms
   - Real issue: queueing from contention

2. **Test realistic scenarios**:
   - Single-pod tests showed good performance
   - Multi-client tests revealed critical failures
   - Both perspectives needed

3. **Session affinity indicates architectural issues**:
   - Need for sticky sessions suggests stateful design
   - May limit horizontal scalability
   - Requires investigation

4. **Small optimizations don't fix architectural problems**:
   - Socket buffers: no impact
   - Real issue: systemic failures under multi-client load
   - Need to fix root cause, not symptoms

---

## Current Status

**Production Readiness**:
- ✅ Single client: YES (100 ops/sec, reliable)
- 🔴 Multiple clients: NO (massive failures)
- 🔴 Cluster-wide scaling: BLOCKED until multi-client issue resolved

**Next Steps**:
1. Deep-dive debugging of multi-client failures
2. Resource usage analysis under load
3. Distributed coordination review
4. Determine if this is fixable or a fundamental design issue

---

**Conclusion**: While we made progress understanding performance characteristics (network is fast, concurrency limits, queueing), we discovered a critical blocker: the system cannot reliably handle multiple concurrent clients. This requires immediate investigation before the system can be considered production-ready for real-world distributed workloads.
