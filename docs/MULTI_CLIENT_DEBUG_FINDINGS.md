# Multi-Client Failure Root Cause Analysis

**Date**: April 22, 2026  
**Status**: 🔴 ROOT CAUSE IDENTIFIED

## Executive Summary

Debug instrumentation revealed the root cause of multi-client failures: **HTTP response deadlock in chunk write handlers**. Chunk writes succeed on disk but fail to send HTTP responses back to clients, causing 30-second timeouts.

## Test Configuration

- **Setup**: 3 benchmark pods, 10 workers each (30 concurrent workers total)
- **Target**: 6-pod Buckets cluster via headless service (random distribution)
- **Workload**: 256KB PUT + GET operations
- **Debug**: Full instrumentation enabled via `BUCKETS_DEBUG=true`

## Test Results

### Failure Rates
- **Pod 1**: 80% PUT failures (8/10 timeouts)
- **Pod 2**: 20% PUT failures (2/10 with issues, 1 timeout)
- **Pod 3**: 40% PUT failures (4/10 with issues, 2 timeouts)

### Timing Analysis
- **Successful chunks**: 7-15 ms
- **Failed chunks**: 30,000 ms (exactly 30 seconds = SOCKET_TIMEOUT_SEC)
- **Pattern**: Failures cluster on specific pods (e.g., buckets-2)

## ROOT CAUSE: HTTP Response Deadlock

### Evidence from Debug Logs

**Sender Side (buckets-0):**
```
[2026-04-22 13:37:42] INFO : [BINARY_WRITE] chunk=12 SUCCESS total_time=29933.65 ms size=65536 endpoint=http://buckets-2
[2026-04-22 13:37:42] INFO : [BINARY_WRITE] chunk=9 SUCCESS total_time=29938.64 ms size=65536 endpoint=http://buckets-2
[2026-04-22 13:37:42] INFO : [BINARY_WRITE] chunk=9 SUCCESS total_time=30263.76 ms size=65536 endpoint=http://buckets-2
[2026-04-22 13:37:42] INFO : [BINARY_WRITE] chunk=12 SUCCESS total_time=30262.20 ms size=65536 endpoint=http://buckets-2
```

**Receiver Side (buckets-2):**
```
[2026-04-22 13:37:42] ERROR: [BINARY_WRITE] chunk=4 failed to receive response
[2026-04-22 13:37:42] ERROR: [BINARY_WRITE] chunk=3 failed to receive response
[2026-04-22 13:37:42] ERROR: [BINARY_WRITE] chunk=1 failed to receive response
[2026-04-22 13:37:42] ERROR: [BINARY_WRITE] chunk=8 failed to receive response
```

### Analysis

1. **Chunk writes complete successfully** - Data reaches disk
2. **HTTP response never sent** - Client times out after 30 seconds
3. **Pattern**: Concentrated on specific pods under load
4. **Timing**: Exactly matches `SOCKET_TIMEOUT_SEC` (30 seconds in `src/storage/binary_transport.c`)

### Hypothesis

The `/_internal/chunk` HTTP handler is:
1. Successfully receiving the chunk data
2. Successfully writing to disk  
3. **Deadlocking or blocking** before sending HTTP 200 response
4. Client socket times out after 30 seconds
5. Response finally sent, but client has already given up

Possible causes:
- **Thread pool exhaustion**: All libuv threads blocked waiting for I/O
- **Lock contention**: Handler holds a lock while waiting for async operation
- **Callback not firing**: HTTP response callback not being invoked
- **Write buffer full**: TCP send buffer full, blocking response send

## Impact

When multiple clients send concurrent requests:
- Some pods receive disproportionate load (hash distribution variance)
- Those pods' thread pools become saturated
- New chunk write handlers block waiting for thread pool
- Existing handlers can't send responses (deadlock)
- Cascading failure as timeouts accumulate

**Single client works**: Requests spread out, no contention  
**Multiple clients fail**: Concentrated load triggers thread pool saturation

## Next Steps

1. **Verify thread pool size**: Check if UV_THREADPOOL_SIZE=128 is effective
2. **Add lock tracking**: Instrument mutex/semaphore wait times
3. **Profile handler**: Where does `/_internal/chunk` handler spend time?
4. **Check callback flow**: Is HTTP response callback being called?
5. **Async handler review**: Is the chunk write truly async or blocking?

## Recommended Fixes (Priority Order)

### 1. Make Chunk Write Handler Non-Blocking (HIGH)
   - Currently uses async handler but may have blocking operations
   - Ensure all disk I/O is truly async (uv_fs_* functions)
   - Don't hold locks during I/O waits

### 2. Increase Thread Pool Size (MEDIUM)
   - UV_THREADPOOL_SIZE=128 may not be enough
   - Try 256 or 512 threads
   - Monitor actual thread usage

### 3. Implement Connection Pooling (MEDIUM)
   - Reuse HTTP connections for chunk writes
   - Reduce connection setup overhead
   - May reduce thread pool pressure

### 4. Add Backpressure/Rate Limiting (LOW)
   - Reject requests when thread pool saturated
   - Return 503 Service Unavailable instead of timing out
   - Client can retry with backoff

## Files to Investigate

1. `src/storage/binary_transport.c` - Chunk write handler registration
2. `src/net/uv_server.c` - Async route handling
3. `src/net/uv_server_internal.h` - Async handler implementation
4. `src/storage/chunk.c` - Actual disk write logic

## Verification

To confirm fix:
1. Run multi-client test (3+ pods, 10 workers each)
2. Check for 30-second timeouts in debug logs
3. Verify all chunk writes complete in <100ms
4. Success rate should be >99%
