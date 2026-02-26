# Migration Engine Architecture

**Document Version**: 1.0  
**Last Updated**: February 25, 2026  
**Status**: IN PROGRESS (Weeks 25-26 complete)  
**Implementation**: Phase 7 (Weeks 25-30)

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture Components](#2-architecture-components)
3. [Scanner (Week 25)](#3-scanner-week-25)
4. [Worker Pool (Week 26)](#4-worker-pool-week-26)
5. [Orchestrator (Week 27)](#5-orchestrator-week-27)
6. [Throttling (Week 28)](#6-throttling-week-28)
7. [Checkpointing (Week 29)](#7-checkpointing-week-29)
8. [Data Flow](#8-data-flow)
9. [Thread Safety](#9-thread-safety)
10. [Performance Characteristics](#10-performance-characteristics)
11. [Error Handling](#11-error-handling)
12. [Testing Strategy](#12-testing-strategy)

---

## 1. Overview

### 1.1 Purpose

The migration engine moves objects between sets when the cluster topology changes (nodes added/removed). It operates in the background without blocking user operations.

### 1.2 Design Goals

- **Non-blocking**: User traffic continues during migration
- **Resumable**: Survive crashes and restarts
- **Efficient**: Minimize unnecessary data movement (consistent hashing)
- **Parallel**: Utilize multiple threads for high throughput
- **Observable**: Clear progress tracking and metrics

### 1.3 Architecture Pattern

```
┌──────────────────────────────────────────────────────────┐
│                   Migration Orchestrator                  │
│  (Job management, state machine, coordination)            │
└───────────────┬──────────────────────────┬────────────────┘
                │                          │
        ┌───────▼────────┐        ┌────────▼────────┐
        │    Scanner     │        │   Worker Pool    │
        │ (Enumerate &   │───────▶│  (Execute        │
        │  Identify)     │ Tasks  │   migrations)    │
        └────────────────┘        └──────────────────┘
                │                          │
                ▼                          ▼
        ┌────────────────┐        ┌────────────────┐
        │  Topology &    │        │  Storage &     │
        │  Hash Rings    │        │  Registry      │
        └────────────────┘        └────────────────┘
```

### 1.4 Implementation Status

| Component | Status | Week | Tests | Lines |
|-----------|--------|------|-------|-------|
| Scanner | ✅ Complete | 25 | 10/10 | 544 |
| Worker Pool | ✅ Complete | 26 | 12/12 | 692 |
| Orchestrator | ⏳ Pending | 27 | 0 | 0 |
| Throttling | ⏳ Pending | 28 | 0 | 0 |
| Checkpointing | ⏳ Pending | 29 | 0 | 0 |
| Integration | ⏳ Pending | 30 | 0 | 0 |

---

## 2. Architecture Components

### 2.1 Component Roles

**Scanner** (`scanner.c`)
- Walks all disks in parallel
- Identifies objects needing migration
- Builds prioritized task queue (small objects first)
- Uses hash rings to determine old/new locations

**Worker Pool** (`worker.c`)
- Thread pool (default: 16 workers)
- Consumes tasks from queue
- Executes 4-step migration per object
- Retry logic with exponential backoff

**Orchestrator** (`orchestrator.c` - Week 27)
- Job lifecycle management
- State machine (IDLE → SCANNING → MIGRATING → COMPLETE)
- Progress tracking and ETA
- Pause/resume capability

**Throttling** (Week 28)
- Bandwidth limiting
- I/O prioritization (user > migration)
- CPU throttling

**Checkpointing** (Week 29)
- Periodic state persistence
- Resume after crash
- Progress recovery

### 2.2 Data Structures

```c
// Migration task (per object)
typedef struct {
    char bucket[256];
    char object[1024];
    char version_id[64];
    
    int old_pool_idx;     // Source location
    int old_set_idx;
    int new_pool_idx;     // Destination location
    int new_set_idx;
    
    i64 size;             // For progress tracking
    time_t mod_time;
    
    int retry_count;      // Retry tracking
    time_t last_attempt;
} buckets_migration_task_t;

// Scanner state
typedef struct {
    char **disk_paths;
    int disk_count;
    
    buckets_cluster_topology_t *old_topology;
    buckets_cluster_topology_t *new_topology;
    
    i64 objects_scanned;
    i64 objects_affected;
    i64 bytes_affected;
    
    pthread_mutex_t lock;
} buckets_scanner_state_t;

// Worker pool (opaque)
typedef struct buckets_worker_pool {
    int num_workers;
    pthread_t *threads;
    task_queue_t *queue;
    
    buckets_cluster_topology_t *old_topology;
    buckets_cluster_topology_t *new_topology;
    
    i64 tasks_completed;
    i64 tasks_failed;
    i64 bytes_migrated;
    i64 active_workers;
    
    pthread_mutex_t stats_lock;
} buckets_worker_pool_t;
```

---

## 3. Scanner (Week 25)

### 3.1 Architecture

**Parallel Per-Disk Scanning:**
- One thread per disk for optimal I/O
- Independent scanning (no coordination needed)
- Results merged at end

```
Disk 1 ─┬─▶ Thread 1 ──▶ Tasks[1..N]
Disk 2 ─┼─▶ Thread 2 ──▶ Tasks[N+1..M]  ─┐
Disk 3 ─┼─▶ Thread 3 ──▶ Tasks[M+1..K]   ├─▶ Merge ──▶ Sort by size
Disk 4 ─┘                                  │
...                                         │
Disk N ────▶ Thread N ──▶ Tasks[...]  ────┘
```

### 3.2 Migration Detection

**Algorithm:**
```c
bool needs_migration(object) {
    // Build hash rings from old and new topologies
    old_ring = topology_to_ring(old_topology);
    new_ring = topology_to_ring(new_topology);
    
    // Compute locations using consistent hashing
    old_node_id = ring_lookup(old_ring, object_key);
    new_node_id = ring_lookup(new_ring, object_key);
    
    // Decode pool/set from node IDs
    old_pool = DECODE_POOL(old_node_id);
    old_set = DECODE_SET(old_node_id);
    new_pool = DECODE_POOL(new_node_id);
    new_set = DECODE_SET(new_node_id);
    
    // Different location = needs migration
    return (old_node_id != new_node_id);
}
```

**Node ID Encoding:**
```c
#define ENCODE_NODE_ID(pool, set) ((pool) * 1000 + (set))
#define DECODE_POOL(node_id) ((node_id) / 1000)
#define DECODE_SET(node_id) ((node_id) % 1000)
```

This encoding allows up to 1000 sets per pool.

### 3.3 Directory Traversal

**Recursive Scan:**
```
/mnt/disk1/
├── bucket1/
│   ├── prefix1/
│   │   ├── object1/
│   │   │   └── xl.meta  ← Found object
│   │   └── object2/
│   │       └── xl.meta  ← Found object
│   └── prefix2/
│       └── ...
└── bucket2/
    └── ...
```

**Implementation:**
```c
void scan_directory(ctx, dir_path, bucket, prefix) {
    DIR *dir = opendir(dir_path);
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            // Recurse into subdirectory
            scan_directory(ctx, full_path, bucket, new_prefix);
        } else if (strcmp(entry->d_name, "xl.meta") == 0) {
            // Found object - check if migration needed
            if (needs_migration(ctx, bucket, prefix)) {
                add_migration_task(ctx, bucket, prefix, ...);
            }
        }
    }
}
```

### 3.4 Task Prioritization

**Small Objects First:**
```c
int compare_tasks_by_size(const void *a, const void *b) {
    const buckets_migration_task_t *task_a = a;
    const buckets_migration_task_t *task_b = b;
    
    if (task_a->size < task_b->size) return -1;
    if (task_a->size > task_b->size) return 1;
    return 0;
}

// Sort after scanning
qsort(tasks, task_count, sizeof(buckets_migration_task_t), 
      compare_tasks_by_size);
```

**Rationale:**
- Small objects migrate quickly → faster progress feedback
- Large objects less likely to fail → save for later
- Better user experience (visible progress sooner)

### 3.5 Performance

**Complexity:**
- Time: O(N × D) where N = objects, D = depth
- Space: O(M) where M = objects needing migration
- Parallelism: P threads (one per disk)

**Typical Performance:**
- 10 million objects, 8 disks
- ~5 minutes to scan
- ~2 million objects need migration (20%)

---

## 4. Worker Pool (Week 26)

### 4.1 Architecture

**Thread Pool Pattern:**
```
                    Task Queue (10K capacity)
                    ┌──────────────────────┐
Producer ──────────▶│ [T1][T2]...[TN]     │
(Scanner)           │  ^head        ^tail  │
                    └────────┬─────────────┘
                             │
                    ┌────────┴─────────────┐
                    │  Condition Variable   │
                    │  (not_empty/not_full) │
                    └─┬──┬──┬──┬──┬──┬──┬─┘
                      │  │  │  │  │  │  │
            ┌─────────┴──┴──┴──┴──┴──┴──┴─────────┐
            │   Worker Threads (16 by default)    │
            └──────────────────────────────────────┘
                      │  │  │  │  │  │  │
                      ▼  ▼  ▼  ▼  ▼  ▼  ▼
                   Migration Execution
```

### 4.2 Task Queue

**Implementation:**
```c
typedef struct {
    buckets_migration_task_t *tasks;  // Circular buffer
    int capacity;                     // Max size (10K)
    int count;                        // Current count
    int head;                         // Consumer position
    int tail;                         // Producer position
    
    pthread_mutex_t lock;             // Mutual exclusion
    pthread_cond_t not_empty;         // Signal for consumers
    pthread_cond_t not_full;          // Signal for producers
    
    bool shutdown;                    // Shutdown flag
} task_queue_t;
```

**Push Operation (Producer):**
```c
int task_queue_push(queue, task) {
    pthread_mutex_lock(&queue->lock);
    
    // Wait if full
    while (queue->count >= queue->capacity && !queue->shutdown) {
        pthread_cond_wait(&queue->not_full, &queue->lock);
    }
    
    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->lock);
        return ERROR;
    }
    
    // Add task
    queue->tasks[queue->tail] = *task;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;
    
    // Wake consumers
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->lock);
    
    return OK;
}
```

**Pop Operation (Consumer):**
```c
bool task_queue_pop(queue, task) {
    pthread_mutex_lock(&queue->lock);
    
    // Wait if empty
    while (queue->count == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->lock);
    }
    
    if (queue->count == 0 && queue->shutdown) {
        pthread_mutex_unlock(&queue->lock);
        return false;  // No more tasks
    }
    
    // Get task
    *task = queue->tasks[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    
    // Wake producers
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);
    
    return true;
}
```

### 4.3 Worker Thread Lifecycle

**Main Loop:**
```c
void* worker_thread_main(void *arg) {
    buckets_worker_pool_t *pool = arg;
    buckets_migration_task_t task;
    
    while (task_queue_pop(pool->queue, &task)) {
        // Mark active
        atomic_increment(&pool->active_workers);
        
        // Execute migration with retries
        execute_migration_with_retry(pool, &task);
        
        // Mark idle
        atomic_decrement(&pool->active_workers);
    }
    
    return NULL;
}
```

**Startup:**
```c
int buckets_worker_pool_start(pool) {
    for (int i = 0; i < pool->num_workers; i++) {
        pthread_create(&pool->threads[i], NULL, 
                      worker_thread_main, pool);
    }
    return OK;
}
```

**Shutdown:**
```c
int buckets_worker_pool_stop(pool) {
    // Signal shutdown
    task_queue_shutdown(pool->queue);
    
    // Wait for all threads
    for (int i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    return OK;
}
```

### 4.4 Migration Execution

**4-Step Process:**
```c
int execute_migration(pool, task) {
    // Step 1: Read object from source
    u8 *data;
    size_t size;
    ret = read_source_object(pool, task, &data, &size);
    if (ret != OK) return ret;
    
    // Step 2: Write to destination
    ret = write_destination_object(pool, task, data, size);
    if (ret != OK) {
        free(data);
        return ret;
    }
    
    // Step 3: Update registry
    ret = update_registry(pool, task);
    if (ret != OK) {
        free(data);
        return ret;
    }
    
    // Step 4: Delete from source (non-fatal)
    ret = delete_source_object(pool, task);
    if (ret != OK) {
        log_warning("Source delete failed (non-fatal)");
    }
    
    free(data);
    
    // Update statistics
    pthread_mutex_lock(&pool->stats_lock);
    pool->tasks_completed++;
    pool->bytes_migrated += task->size;
    pthread_mutex_unlock(&pool->stats_lock);
    
    return OK;
}
```

### 4.5 Retry Logic

**Exponential Backoff:**
```c
int execute_migration_with_retry(pool, task) {
    int attempts = 0;
    int delay_ms = 100;  // Initial delay
    
    while (attempts < 3) {
        int ret = execute_migration(pool, task);
        
        if (ret == OK) {
            return OK;  // Success
        }
        
        attempts++;
        task->retry_count = attempts;
        
        if (attempts < 3) {
            // Exponential backoff
            struct timespec ts;
            ts.tv_sec = delay_ms / 1000;
            ts.tv_nsec = (delay_ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
            
            delay_ms *= 2;  // Double delay
            if (delay_ms > 5000) {
                delay_ms = 5000;  // Cap at 5s
            }
        }
    }
    
    // All retries exhausted
    pthread_mutex_lock(&pool->stats_lock);
    pool->tasks_failed++;
    pthread_mutex_unlock(&pool->stats_lock);
    
    return ERROR;
}
```

**Retry Schedule:**
- Attempt 1: Immediate
- Attempt 2: Wait 100ms
- Attempt 3: Wait 200ms (if attempt 2 fails)
- Max delay: 5000ms

### 4.6 Progress Tracking

**Statistics:**
```c
typedef struct {
    i64 tasks_completed;   // Successfully migrated
    i64 tasks_failed;      // Failed after retries
    i64 bytes_migrated;    // Total bytes transferred
    i64 active_workers;    // Currently executing
    i64 idle_workers;      // Waiting for tasks
    double throughput_mbps; // Current MB/s
} buckets_worker_stats_t;
```

**Throughput Calculation:**
```c
int buckets_worker_pool_get_stats(pool, stats) {
    pthread_mutex_lock(&pool->stats_lock);
    
    stats->tasks_completed = pool->tasks_completed;
    stats->tasks_failed = pool->tasks_failed;
    stats->bytes_migrated = pool->bytes_migrated;
    stats->active_workers = pool->active_workers;
    stats->idle_workers = pool->num_workers - pool->active_workers;
    
    // Calculate throughput
    time_t elapsed = time(NULL) - pool->start_time;
    if (elapsed > 0) {
        double mb = (double)pool->bytes_migrated / (1024.0 * 1024.0);
        stats->throughput_mbps = mb / (double)elapsed;
    }
    
    pthread_mutex_unlock(&pool->stats_lock);
    
    return OK;
}
```

---

## 5. Orchestrator (Week 27)

### 5.1 Overview

**Responsibilities:**
- Job lifecycle management
- State machine coordination
- Integration of scanner + workers
- Progress reporting
- Pause/resume

### 5.2 State Machine

```
┌──────┐
│ IDLE │
└───┬──┘
    │ start_migration()
    ▼
┌──────────┐
│SCANNING  │ (Scanner enumerating objects)
└────┬─────┘
     │ scan_complete()
     ▼
┌───────────┐
│MIGRATING  │ (Workers processing tasks)
└─────┬─────┘
      │ pause()          ├─────────┐
      ▼                  │         │
  ┌────────┐             │         │
  │ PAUSED │─────────────┘         │
  └────────┘ resume()              │
      │                            │
      │ all_tasks_complete()       │ error()
      ▼                            ▼
┌───────────┐              ┌─────────┐
│ COMPLETED │              │ FAILED  │
└───────────┘              └─────────┘
```

**Implementation** (planned):
```c
typedef enum {
    STATE_IDLE,
    STATE_SCANNING,
    STATE_MIGRATING,
    STATE_PAUSED,
    STATE_COMPLETED,
    STATE_FAILED
} migration_state_t;

typedef struct {
    char job_id[64];
    migration_state_t state;
    
    buckets_scanner_state_t *scanner;
    buckets_worker_pool_t *worker_pool;
    
    i64 total_objects;
    i64 migrated_objects;
    i64 failed_objects;
    
    time_t start_time;
    time_t estimated_completion;
    
    pthread_mutex_t lock;
} migration_job_t;
```

---

## 6. Throttling (Week 28)

### 6.1 Bandwidth Limiting

**Goal**: Prevent migration from saturating network

**Approach**: Token bucket algorithm
```c
typedef struct {
    i64 tokens;              // Available tokens (bytes)
    i64 capacity;            // Max tokens (burst)
    i64 rate;                // Refill rate (bytes/sec)
    time_t last_refill;      // Last refill time
    
    pthread_mutex_t lock;
} token_bucket_t;

void throttle_wait(bucket, bytes) {
    pthread_mutex_lock(&bucket->lock);
    
    // Refill tokens based on elapsed time
    time_t now = time(NULL);
    i64 elapsed = now - bucket->last_refill;
    bucket->tokens += elapsed * bucket->rate;
    if (bucket->tokens > bucket->capacity) {
        bucket->tokens = bucket->capacity;
    }
    bucket->last_refill = now;
    
    // Wait until enough tokens available
    while (bucket->tokens < bytes) {
        pthread_mutex_unlock(&bucket->lock);
        usleep(10000);  // 10ms
        pthread_mutex_lock(&bucket->lock);
        // Refill again...
    }
    
    // Consume tokens
    bucket->tokens -= bytes;
    pthread_mutex_unlock(&bucket->lock);
}
```

### 6.2 I/O Prioritization

**Priority Levels:**
1. User traffic (highest)
2. Healing/verification
3. Migration (lowest)

**Implementation**: Separate I/O queues with priority scheduling

---

## 7. Checkpointing (Week 29)

### 7.1 Checkpoint Structure

```json
{
  "job_id": "migration-gen-42-to-43",
  "state": "MIGRATING",
  "checkpoint_time": 1708896000,
  
  "total_objects": 2000000,
  "completed_objects": 500000,
  "failed_objects": 123,
  
  "last_bucket": "my-bucket",
  "last_object": "photos/2024/IMG_5678.jpg",
  
  "source_generation": 42,
  "target_generation": 43
}
```

### 7.2 Checkpoint Triggers

- Every 1000 objects migrated
- Every 5 minutes
- Before graceful shutdown
- On SIGTERM/SIGINT

### 7.3 Recovery

```c
int resume_from_checkpoint(checkpoint_path) {
    // 1. Load checkpoint
    checkpoint = load_checkpoint(checkpoint_path);
    
    // 2. Verify topology hasn't changed
    if (checkpoint.target_generation != current_generation) {
        return ERROR_TOPOLOGY_CHANGED;
    }
    
    // 3. Rebuild scanner state
    scanner = buckets_scanner_init(...);
    scanner->last_bucket = checkpoint.last_bucket;
    scanner->last_object = checkpoint.last_object;
    
    // 4. Resume scanning from checkpoint
    buckets_scanner_scan(scanner, &tasks, &count);
    
    // 5. Submit remaining tasks to workers
    buckets_worker_pool_submit(pool, tasks, count);
    
    return OK;
}
```

---

## 8. Data Flow

### 8.1 Complete Migration Flow

```
1. Topology Change Detected
   └─▶ Create migration job
       └─▶ Initialize scanner with old/new topologies
           └─▶ Start scanner threads (one per disk)
               └─▶ Walk directories, find xl.meta files
                   └─▶ Check if object needs migration
                       └─▶ Add to task queue (sorted by size)
                           └─▶ Submit tasks to worker pool
                               └─▶ Workers pop tasks from queue
                                   └─▶ Execute 4-step migration
                                       ├─▶ Read from source
                                       ├─▶ Write to destination
                                       ├─▶ Update registry
                                       └─▶ Delete from source
                                   └─▶ Update statistics
                                   └─▶ Checkpoint periodically
                               └─▶ All tasks complete
                           └─▶ Mark job COMPLETED
```

### 8.2 Concurrent User Operations

**READ during migration:**
```
1. Check registry for current location
2. If found, read from that location
3. If registry miss:
   a. Compute expected location (new topology)
   b. Try reading from computed location
   c. If not found, fan out to old location (migration in progress)
```

**WRITE during migration:**
```
1. Always write to new location (current generation)
2. Migration only touches objects from before topology change
3. No conflicts possible
```

**DELETE during migration:**
```
1. Delete from both old and new locations (idempotent)
2. Update registry to mark deleted
3. Migration will skip if it encounters missing object
```

---

## 9. Thread Safety

### 9.1 Locking Hierarchy

**Level 1**: Scanner lock (coarse-grained)
- Protects scanner state
- Held briefly during stats updates

**Level 2**: Worker pool stats lock
- Protects global counters
- Held briefly during stat updates

**Level 3**: Task queue lock
- Protects queue data structure
- Held during push/pop operations
- Condition variables for blocking

**Rules:**
- Never hold multiple locks simultaneously
- Always release in reverse order of acquisition
- Use condition variables to avoid busy-waiting

### 9.2 Lock-Free Statistics

**Approach**: Use atomic operations where possible
```c
// Instead of:
pthread_mutex_lock(&lock);
counter++;
pthread_mutex_unlock(&lock);

// Use:
__atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
```

**Note**: Current implementation uses mutexes for portability

---

## 10. Performance Characteristics

### 10.1 Scanner Performance

**Metrics** (10M objects, 8 disks):
- Scan time: ~5 minutes
- Memory: ~500 MB (2M tasks × 250 bytes/task)
- CPU: Low (I/O bound)
- Disk I/O: Sequential reads (efficient)

### 10.2 Worker Pool Performance

**Metrics** (16 workers, 1 Gbps network):
- Throughput: ~125 MB/s (network limited)
- Latency per object: ~50-100ms
- CPU: Medium (depends on object size)
- Memory: ~10 MB queue + worker stacks

### 10.3 Expected Migration Times

**Small deployment** (1M objects, 100GB):
- Scan: 30 seconds
- Migrate: 15 minutes
- **Total**: 16 minutes

**Medium deployment** (10M objects, 1TB):
- Scan: 5 minutes
- Migrate: 2 hours
- **Total**: 2h 5m

**Large deployment** (100M objects, 10TB):
- Scan: 50 minutes
- Migrate: 20 hours
- **Total**: ~21 hours

---

## 11. Error Handling

### 11.1 Transient Errors

**Network timeouts, disk I/O errors:**
- Retry up to 3 times with exponential backoff
- Log warnings on failure
- Mark task as failed after exhausting retries

### 11.2 Permanent Errors

**Source object missing:**
- Skip and continue (object may have been deleted)
- Log info message
- Don't mark as failure

**Destination write failure:**
- Retry with backoff
- If all retries fail, mark as failed
- Continue with other objects

**Registry update failure:**
- Retry (critical operation)
- If fails, mark object migration as incomplete
- Can be reprocessed later

### 11.3 Fatal Errors

**Topology changed during migration:**
- Abort current migration
- Log error
- Require operator intervention

**Out of disk space:**
- Pause migration
- Alert operator
- Can resume after space freed

---

## 12. Testing Strategy

### 12.1 Unit Tests

**Scanner** (10 tests):
- Empty cluster
- Single/multiple pools
- Statistics accuracy
- Task sorting
- Large object counts

**Worker Pool** (12 tests):
- Pool creation/destruction
- Thread lifecycle
- Task submission
- Statistics tracking
- Error handling
- Large batches

### 12.2 Integration Tests (Week 30)

**Scenarios:**
- Full migration workflow
- Crash and resume
- Concurrent user traffic
- Multiple topology changes
- Resource limits

### 12.3 Performance Tests (Week 30)

**Benchmarks:**
- Scanner throughput (objects/sec)
- Worker throughput (MB/s)
- Memory usage under load
- CPU utilization
- Network saturation behavior

---

## References

- `include/buckets_migration.h` - Public API
- `src/migration/scanner.c` - Scanner implementation (544 lines)
- `src/migration/worker.c` - Worker pool implementation (692 lines)
- `tests/migration/test_scanner.c` - Scanner tests (10 tests)
- `tests/migration/test_worker.c` - Worker tests (12 tests)
- `architecture/SCALE_AND_DATA_PLACEMENT.md` - Hash ring design
- `ROADMAP.md` - Implementation timeline

---

**Document Status**: Living document, updated as implementation progresses  
**Next Update**: After Week 27 (Orchestrator implementation)
