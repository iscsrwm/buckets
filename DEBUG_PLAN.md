# Multi-Client Failure Debugging Plan

**Date**: April 22, 2026  
**Status**: In Progress

---

## Findings So Far

### 1. Error Pattern
- Error: "Resource temporarily unavailable" (EAGAIN) during TCP connect()
- Occurs when making RPC calls from buckets-0 to buckets-5
- Happens under concurrent load (10+ workers)

### 2. Resource Usage
- File descriptors: Stable at ~458 total (NOT a leak)
- Processes: 17 (1 master + 16 workers)
- No obvious resource exhaustion

### 3. Reproducibility
- Single client, low concurrency (2-5 workers): **Works fine** (100% success)
- Single client, medium concurrency (10 workers): **Some failures** (~3%)
- Multiple clients (3-6 pods, 10 workers each): **Massive failures** (40-70%)

---

## Hypothesis

The "Resource temporarily unavailable" during `connect()` suggests one of:

### Hypothesis A: Listen Backlog Full
- Each pod's listen backlog (somaxconn=4096) might be too small
- When 3-6 clients (30-60 workers) all try to connect simultaneously
- Incoming connections queue up faster than they can be accepted
- **Test**: Increase listen backlog

### Hypothesis B: Ephemeral Port Exhaustion
- Each worker makes many RPC connections
- Port range: 32768-60999 (~28K ports)
- With TIME_WAIT, ports may be exhausted
- **Test**: Check TIME_WAIT sockets, increase port range

### Hypothesis C: Connection Pool Saturation
- Connection pool might not be handling concurrent requests well
- Race conditions when multiple threads try to get/return connections
- **Test**: Add connection pool statistics/logging

### Hypothesis D: Target Pod Overload
- The target pod (buckets-5) is overwhelmed
- Worker threads all busy, can't accept new connections fast enough
- **Test**: Check if failures correlate with specific target pods

---

## Debug Actions

### Action 1: Increase Listen Backlog ⭐
The listen backlog of 4096 might be too small for bursty concurrent connections.

**Change**:
```c
// In uv_server.c or wherever listen() is called
listen(sockfd, 8192);  // Increase from 4096
```

Or increase system limit:
```bash
sysctl -w net.core.somaxconn=8192
```

**Expected**: If this is the issue, failures should decrease significantly.

### Action 2: Add Connection Pool Diagnostics
Add logging to see connection pool behavior:
- How many connections are created vs reused?
- Are there lock contention issues?
- Connection creation failures?

### Action 3: Check libuv Accept Queue
The libuv server might not be accepting connections fast enough.
- Check UV_THREADPOOL_SIZE (currently 64)
- Check if event loop is blocked

### Action 4: Test with Connection Limits
Artificially limit concurrent connections to see behavior:
- Set max_conns in connection pool
- See if limiting helps or hurts

### Action 5: Enable TCP Keepalive
Connections might be timing out silently:
```c
int keepalive = 1;
setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
```

---

## Immediate Next Step

**Try increasing listen backlog** - This is the most likely cause given the error pattern.

The fact that it's specifically "Resource temporarily unavailable" during `connect()` strongly suggests the server's listen queue is full.

---

## Test Plan

1. Modify listen backlog to 8192 or higher
2. Rebuild and deploy
3. Run 3-pod concurrent test
4. Compare failure rate

If successful, we'll see:
- Failure rate drop from 40-70% to <5%
- No more "Resource temporarily unavailable" errors
- Better throughput under concurrent load

---

**Status**: Ready to implement listen backlog increase
