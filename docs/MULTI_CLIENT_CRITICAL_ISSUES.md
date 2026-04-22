# Multi-Client Critical Issues - Thread Pool Increase Insufficient

**Date**: April 22, 2026  
**Status**: 🔴 CRITICAL BUGS DISCOVERED

## Executive Summary

Increasing `UV_THREADPOOL_SIZE` from 128 to 512 is **NOT sufficient** to fix multi-client failures. Testing revealed **critical bugs**:

1. **Worker Process Crashes** (SIGSEGV)
2. **Memory Corruption** (Invalid stream type errors)
3. **Race Conditions** (Use-after-free in UV server)

## Test Results with UV_THREADPOOL_SIZE=512

**Configuration:**
- 3 concurrent clients
- 20 workers per client (60 total concurrent workers)
- 256KB PUT operations
- 30-second duration

**Results:**
| Client | Success Rate | Failures | Avg Latency | Throughput |
|--------|--------------|----------|-------------|------------|
| Client 1 | 42% (15/36) | 21 | 17,009ms | 0.43 ops/sec |
| Client 2 | 34% (13/38) | 25 | 16,721ms | 0.36 ops/sec |
| Client 3 | 15% (4/27) | 23 | 27,393ms | 0.07 ops/sec |
| **Overall** | **30% (32/101)** | **69 failures** | **20,000ms** | **0.29 ops/sec** |

**Comparison to Previous Test (with simpler curl-based test):**
- Previous (simple PUT/GET): 96.7% success rate
- Current (sustained load): 30% success rate
- **Conclusion**: Sustained multi-client load exposes critical bugs

## Root Cause Analysis

### 1. Worker Crashes (SIGSEGV)

**Evidence:**
```
[2026-04-22 14:40:59] WARN : Worker 0 (pid=11) killed by signal 11
```

**Signal 11 = SIGSEGV** - Segmentation fault (invalid memory access)

**Possible Causes:**
- NULL pointer dereference
- Use-after-free
- Buffer overflow
- Stack corruption

### 2. Memory Corruption

**Evidence:**
```
[2026-04-22 14:40:59] ERROR: Invalid stream type 0 (expected UV_TCP=12)
```

**Analysis:**
- `stream->type` should be `UV_TCP (12)` but is `0`
- Indicates memory has been zeroed/freed
- Classic use-after-free pattern

**Code Location:**
Likely in `src/net/uv_server.c` - safe_uv_write or connection handling

### 3. HTTP Response Parsing Failures

**Evidence:**
```
[2026-04-22 14:40:59] ERROR: [BINARY_WRITE] chunk=5 failed to parse HTTP response
[2026-04-22 14:41:03] ERROR: [BINARY_WRITE] chunk=7 remote write failed with status 400
```

**Analysis:**
- Binary chunk writes receiving invalid HTTP responses
- Could be due to worker crash mid-response
- Or race condition in response writing

## Why Simple Tests Passed But Sustained Load Fails

**Simple PUT/GET Test (96.7% success):**
- Short duration (10 operations per worker)
- Low sustained concurrency
- Enough time between requests for cleanup
- Bugs don't have time to accumulate

**Sustained Load Test (30% success):**
- Longer duration (30+ seconds)
- High sustained concurrency (60 workers)
- Memory leaks/corruption accumulate
- Race conditions more likely to trigger
- Worker crashes cascade to failures

## Impact

**System is NOT production-ready for sustained multi-client workloads**

The underlying issues:
- Worker process stability (crashes under load)
- Memory safety (use-after-free bugs)
- Concurrency correctness (race conditions)

These are **critical bugs** that must be fixed before deployment.

## Next Steps - Priority Order

### 1. Fix Worker Crashes (CRITICAL)
- Enable core dumps to analyze crash
- Run with AddressSanitizer to catch memory errors
- Identify crash location and fix

### 2. Fix Memory Corruption (CRITICAL)
- Investigate "Invalid stream type 0" error
- Review connection lifecycle management
- Ensure proper reference counting

### 3. Fix HTTP Response Race Conditions (HIGH)
- Review async response handling
- Ensure proper synchronization
- Add connection state validation

### 4. Add Comprehensive Testing (HIGH)
- Memory leak detection (Valgrind)
- Race condition detection (ThreadSanitizer)
- Stress testing under sustained load

## Recommended Immediate Actions

1. **DO NOT deploy to production** - System unstable under load
2. **Enable debugging tools** - AddressSanitizer, core dumps
3. **Reproduce crash locally** - Use same multi-client test
4. **Fix memory bugs** - Use-after-free, race conditions
5. **Retest** - Verify fixes with sustained load

## Temporary Mitigation (Not Recommended for Production)

If production deployment is urgent:
- **Limit concurrent clients** - Single client at a time
- **Enable automatic worker restart** - Worker pool handles crashes
- **Short-lived connections** - Minimize memory leak impact
- **Monitor and alert** - Watch for worker crashes

**WARNING**: These are **workarounds**, not fixes. The underlying bugs remain.

## Files to Investigate

1. `src/net/uv_server.c` - Connection lifecycle, safe_uv_write
2. `src/net/uv_server_internal.h` - Connection state management
3. `src/storage/binary_transport.c` - Chunk write HTTP handling
4. `src/net/worker_pool.c` - Worker process management

## Conclusion

Increasing thread pool size **addresses the symptom** (thread exhaustion) but **does not fix the disease** (memory corruption and worker crashes).

The system requires:
- Memory safety fixes (use-after-free, race conditions)
- Worker stability (no crashes under sustained load)
- Proper concurrency handling

**Estimated time to fix**: 1-2 days of debugging + testing

**Current Status**: NOT production-ready for multi-client sustained load
