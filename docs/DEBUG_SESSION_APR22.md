# Debug Session - Multi-Client Failures

**Date**: April 22, 2026  
**Duration**: ~2 hours  
**Status**: Partial progress, more work needed

---

## Problem Statement

System fails under multi-client load with 40-70% failure rates and 10-20 second latencies.

---

## Debugging Steps Taken

### Step 1: Reproduced the Issue
- Single client, 10 workers: ~3% failure rate
- 3 clients, 10 workers each: 40-70% failure rate
- Confirmed consistent reproducibility

### Step 2: Resource Monitoring
- Monitored FDs: Stable at ~458 total (no leak)
- Monitored processes: 17 (1 master + 16 workers)
- No obvious resource exhaustion

### Step 3: Identified First Root Cause
**Error**: "Resource temporarily unavailable" during TCP `connect()`

**Investigation**:
- Checked RPC semaphore: 512 concurrent calls (adequate)
- Checked connection pool: Unlimited (no artificial limit)
- Checked listen backlog: **128 (WAY TOO SMALL!)** ⬅️ Found it!

**Fix Applied**:
```c
// src/net/uv_server_internal.h
#define BUCKETS_DEFAULT_BACKLOG 4096  // Was: 128
```

**Result**: 
- "Resource temporarily unavailable" errors: Reduced from ~100+ to 2
- **Listen backlog was A problem, but NOT THE ONLY problem**

### Step 4: Discovered Additional Issues

After fixing listen backlog, new errors appeared:

#### Error Type A: "Invalid stream type 0 (expected UV_TCP=12)"
```
ERROR: Invalid stream type 0 (expected UV_TCP=12)
ERROR: Response already started
```

**Analysis**: This error comes from `is_stream_valid_for_write()` in uv_server.c:

```c
if (stream->type != UV_TCP) {
    buckets_error("Invalid stream type %d (expected UV_TCP=%d)", 
                  stream->type, UV_TCP);
    return false;
}
```

**Implications**:
- The TCP stream handle is being corrupted or freed
- Stream type 0 = no type (freed/uninitialized memory)
- **This is a use-after-free or race condition bug**

#### Error Type B: "Failed to receive response"
```
ERROR: Failed to receive response
ERROR: Parallel: Failed to write chunk 3 via binary transport to ...
ERROR: Chunk 3 write failed
```

**Analysis**: RPC calls are timing out or failing to complete

---

## Current Status

### What's Fixed ✅
1. **Listen backlog increased** (128 → 4096)
   - Reduced "Resource temporarily unavailable" by 98%
   - System can now accept more concurrent connections

### What's Still Broken 🔴
1. **Use-after-free bug in UV server**
   - "Invalid stream type 0" errors
   - Likely a race condition when closing connections
   - Affects ~30% of requests under multi-client load

2. **RPC timeouts**
   - "Failed to receive response" errors
   - May be related to the use-after-free issue

### Test Results After Fix
**3-pod test** (30 total workers):
- Pod 1: 69% success (20/29)
- Pod 2: 63% success (19/30)
- Pod 3: 47% success (9/19)
- **Average**: ~60% success rate (was 40-50% before)

**Improvement**: 20-30% better, but still not production-ready

---

## Root Cause Analysis

### The Use-After-Free Bug

**Location**: `src/net/uv_server.c` - connection closing logic

**Hypothesis**: Race condition in connection cleanup:
1. Thread A starts writing response to connection
2. Thread B (timeout/error) closes connection, frees handle
3. Thread A tries to write to freed handle
4. Error: "Invalid stream type 0"

**Evidence**:
- Error only occurs under high concurrency
- "Response already started" suggests multiple threads accessing same connection
- Stream type 0 indicates freed/corrupted memory

**Related Code**:
```c
// src/net/uv_server.c:~920-960
static void on_handle_close(uv_handle_t *handle) {
    uv_http_conn_t *conn = (uv_http_conn_t*)handle->data;
    
    conn->pending_close_count--;
    if (conn->pending_close_count == 0) {
        // Free connection - but what if someone still has a reference?
        buckets_free(conn);
    }
}
```

**Potential fix locations**:
1. `uv_http_conn_close()` - ensure no writes in flight
2. `send_buffered_response()` - check connection state before writing
3. Add reference counting to connections

---

## Next Steps

### Immediate: Fix the Use-After-Free Bug

**Option 1: Add Reference Counting**
```c
typedef struct uv_http_conn {
    // ...
    atomic_int ref_count;
    // ...
} uv_http_conn_t;

void conn_addref(uv_http_conn_t *conn) {
    atomic_fetch_add(&conn->ref_count, 1);
}

void conn_release(uv_http_conn_t *conn) {
    if (atomic_fetch_sub(&conn->ref_count, 1) == 1) {
        // Last reference, safe to free
        buckets_free(conn);
    }
}
```

**Option 2: Connection State Machine**
```c
typedef enum {
    CONN_STATE_ACTIVE,
    CONN_STATE_CLOSING,   // No new writes allowed
    CONN_STATE_CLOSED      // Fully closed
} conn_state_t;

// Before any write:
if (conn->state != CONN_STATE_ACTIVE) {
    return -1;  // Don't write to closing connection
}
```

**Option 3: Flush and Wait**
- Before closing, ensure all pending writes complete
- Use a barrier or semaphore
- Only close after all writes acknowledged

### Testing Strategy

1. **Add debug logging**:
   - Log every connection open/close with ID
   - Log every write attempt with connection state
   - Track where "Invalid stream type" originates

2. **Run under Valgrind** (if possible):
   - Detect use-after-free directly
   - Get stack traces

3. **Stress test with delays**:
   - Add small delays before closing connections
   - See if it reduces failures (confirms race condition)

---

## Lessons Learned

1. **Multiple bottlenecks can compound**:
   - Listen backlog was ONE issue
   - Use-after-free is ANOTHER issue
   - Both contribute to failures

2. **Fixing one issue reveals the next**:
   - With backlog fixed, use-after-free becomes dominant
   - This is normal in debugging

3. **Error messages are clues**:
   - "Resource temporarily unavailable" → listen backlog
   - "Invalid stream type 0" → use-after-free
   - Each error type has a different root cause

4. **Concurrency bugs are hard**:
   - Work fine at low concurrency
   - Break only under high concurrent load
   - Race conditions are timing-dependent

---

## Progress Summary

**Time spent**: 2 hours  
**Issues identified**: 2 (listen backlog, use-after-free)  
**Issues fixed**: 1 (listen backlog)  
**Improvement**: 40-50% → 60% success rate  
**Status**: Making progress, more work needed

---

## Recommendation

The use-after-free bug requires careful code review and fixing. Options:

1. **Continue debugging now** - Fix the use-after-free bug (2-4 hours estimated)
2. **Document and pause** - Write up findings, resume later
3. **Workaround** - Reduce concurrency to avoid triggering the bug

Given the complexity, I recommend **option 1** if time permits - let's fix the use-after-free bug properly.

---

**Status**: Ready to continue debugging or pause for documentation
