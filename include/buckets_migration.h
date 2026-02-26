/**
 * Migration Engine API
 * 
 * Background data migration on topology changes.
 * 
 * When cluster topology changes (nodes added/removed), objects need to be
 * migrated to their new computed locations. This module provides:
 * 
 * - Scanner: Enumerate objects and identify what needs migration
 * - Workers: Parallel object movement (16 threads)
 * - Orchestrator: Job management and coordination
 * - Checkpointing: Resume capability after crashes
 * - Throttling: Bandwidth and I/O control
 */

#ifndef BUCKETS_MIGRATION_H
#define BUCKETS_MIGRATION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "buckets.h"
#include "buckets_cluster.h"

/* ===================================================================
 * Migration States
 * ===================================================================*/

typedef enum {
    BUCKETS_MIGRATION_STATE_IDLE = 0,       /* No migration active */
    BUCKETS_MIGRATION_STATE_SCANNING = 1,   /* Enumerating objects */
    BUCKETS_MIGRATION_STATE_MIGRATING = 2,  /* Moving objects */
    BUCKETS_MIGRATION_STATE_PAUSED = 3,     /* Temporarily paused */
    BUCKETS_MIGRATION_STATE_COMPLETED = 4,  /* Successfully completed */
    BUCKETS_MIGRATION_STATE_FAILED = 5      /* Failed with errors */
} buckets_migration_state_t;

/* ===================================================================
 * Migration Task
 * ===================================================================*/

/**
 * Per-object migration task
 */
typedef struct {
    char bucket[256];           /* Bucket name */
    char object[1024];          /* Object key */
    char version_id[64];        /* Version ID (empty for non-versioned) */
    
    int old_pool_idx;           /* Source pool index */
    int old_set_idx;            /* Source set index */
    int new_pool_idx;           /* Destination pool index */
    int new_set_idx;            /* Destination set index */
    
    i64 size;                   /* Object size in bytes (for progress) */
    time_t mod_time;            /* Modification time (for conflict resolution) */
    
    int retry_count;            /* Number of retry attempts */
    time_t last_attempt;        /* Timestamp of last attempt */
} buckets_migration_task_t;

/* ===================================================================
 * Scanner State
 * ===================================================================*/

/**
 * Scanner progress tracking
 */
typedef struct {
    char **disk_paths;          /* Array of disk paths to scan */
    int disk_count;             /* Number of disks */
    
    /* Topologies for migration detection */
    buckets_cluster_topology_t *old_topology;  /* Source topology */
    buckets_cluster_topology_t *new_topology;  /* Target topology */
    
    i64 objects_scanned;        /* Total objects scanned */
    i64 objects_affected;       /* Objects needing migration */
    i64 bytes_affected;         /* Total bytes to migrate */
    
    /* Checkpoint support - resume from last position */
    char *last_bucket;          /* Last bucket scanned */
    char *last_object;          /* Last object scanned */
    bool scan_complete;         /* Scanning finished? */
    
    pthread_mutex_t lock;       /* Thread safety */
} buckets_scanner_state_t;

/**
 * Scanner statistics
 */
typedef struct {
    i64 objects_scanned;
    i64 objects_affected;
    i64 bytes_affected;
    double progress_percent;    /* 0.0 to 100.0 */
    bool complete;
} buckets_scanner_stats_t;

/* ===================================================================
 * Migration Job
 * ===================================================================*/

/* Forward declarations */
typedef struct buckets_migration_job buckets_migration_job_t;
typedef struct buckets_worker_pool buckets_worker_pool_t;

/**
 * Event callback function type
 * 
 * Called when job state changes or progress updates occur.
 * 
 * @param job Job handle
 * @param event_type Event type (e.g., "state_change", "progress")
 * @param user_data User-provided data
 */
typedef void (*buckets_migration_event_callback_t)(buckets_migration_job_t *job,
                                                     const char *event_type,
                                                     void *user_data);

/**
 * Top-level migration job
 */
struct buckets_migration_job {
    char job_id[64];                        /* "migration-gen-42-to-43" */
    i64 source_generation;                  /* Old topology generation */
    i64 target_generation;                  /* New topology generation */
    
    time_t start_time;                      /* Job start timestamp */
    time_t estimated_completion;            /* ETA */
    
    buckets_migration_state_t state;        /* Current state */
    
    /* Progress tracking */
    i64 total_objects;                      /* Total to migrate */
    i64 migrated_objects;                   /* Successfully migrated */
    i64 failed_objects;                     /* Failed migrations */
    i64 bytes_total;                        /* Total bytes */
    i64 bytes_migrated;                     /* Bytes migrated so far */
    
    /* Topology snapshots */
    buckets_cluster_topology_t *old_topology;  /* Source topology */
    buckets_cluster_topology_t *new_topology;  /* Target topology */
    
    /* Disk paths */
    char **disk_paths;
    int disk_count;
    
    /* Components */
    buckets_scanner_state_t *scanner;       /* Scanner (SCANNING state) */
    buckets_worker_pool_t *worker_pool;     /* Worker pool (MIGRATING state) */
    
    /* Event callback */
    buckets_migration_event_callback_t callback;
    void *callback_user_data;
    
    pthread_mutex_t lock;                   /* Thread safety */
};

/* ===================================================================
 * Scanner API
 * ===================================================================*/

/**
 * Initialize scanner
 * 
 * @param disk_paths Array of disk paths to scan
 * @param disk_count Number of disks
 * @param old_topology Source topology (before change)
 * @param new_topology Target topology (after change)
 * @return Scanner state or NULL on error
 */
buckets_scanner_state_t* buckets_scanner_init(char **disk_paths, int disk_count,
                                               buckets_cluster_topology_t *old_topology,
                                               buckets_cluster_topology_t *new_topology);

/**
 * Scan disks and build migration queue
 * 
 * Walks all disks in parallel, identifies objects needing migration,
 * and adds them to the task queue.
 * 
 * @param scanner Scanner state
 * @param queue Output queue for migration tasks
 * @param queue_size Maximum queue size
 * @param task_count Output: number of tasks added
 * @return BUCKETS_OK on success, error code on failure
 */
int buckets_scanner_scan(buckets_scanner_state_t *scanner,
                         buckets_migration_task_t **queue,
                         int *queue_size,
                         int *task_count);

/**
 * Get scanner statistics
 * 
 * @param scanner Scanner state
 * @param stats Output statistics
 * @return BUCKETS_OK on success
 */
int buckets_scanner_get_stats(buckets_scanner_state_t *scanner,
                               buckets_scanner_stats_t *stats);

/**
 * Cleanup scanner
 * 
 * @param scanner Scanner state
 */
void buckets_scanner_cleanup(buckets_scanner_state_t *scanner);

/* ===================================================================
 * Worker Pool
 * ===================================================================*/

/**
 * Worker statistics
 */
typedef struct {
    i64 tasks_completed;        /* Tasks successfully migrated */
    i64 tasks_failed;           /* Tasks that failed after retries */
    i64 bytes_migrated;         /* Total bytes migrated */
    i64 active_workers;         /* Workers currently processing */
    i64 idle_workers;           /* Workers waiting for tasks */
    double throughput_mbps;     /* Current throughput in MB/s */
} buckets_worker_stats_t;

/* ===================================================================
 * Worker Pool API
 * ===================================================================*/

/**
 * Create worker pool
 * 
 * @param num_workers Number of worker threads (default: 16)
 * @param old_topology Source topology
 * @param new_topology Target topology
 * @param disk_paths Array of disk paths
 * @param disk_count Number of disks
 * @return Worker pool or NULL on error
 */
buckets_worker_pool_t* buckets_worker_pool_create(int num_workers,
                                                    buckets_cluster_topology_t *old_topology,
                                                    buckets_cluster_topology_t *new_topology,
                                                    char **disk_paths,
                                                    int disk_count);

/**
 * Start worker threads
 * 
 * @param pool Worker pool
 * @return BUCKETS_OK on success
 */
int buckets_worker_pool_start(buckets_worker_pool_t *pool);

/**
 * Submit tasks to worker pool
 * 
 * @param pool Worker pool
 * @param tasks Array of migration tasks
 * @param task_count Number of tasks
 * @return BUCKETS_OK on success
 */
int buckets_worker_pool_submit(buckets_worker_pool_t *pool,
                                 buckets_migration_task_t *tasks,
                                 int task_count);

/**
 * Wait for all tasks to complete
 * 
 * @param pool Worker pool
 * @return BUCKETS_OK on success
 */
int buckets_worker_pool_wait(buckets_worker_pool_t *pool);

/**
 * Stop worker pool (graceful shutdown)
 * 
 * @param pool Worker pool
 * @return BUCKETS_OK on success
 */
int buckets_worker_pool_stop(buckets_worker_pool_t *pool);

/**
 * Get worker statistics
 * 
 * @param pool Worker pool
 * @param stats Output statistics
 * @return BUCKETS_OK on success
 */
int buckets_worker_pool_get_stats(buckets_worker_pool_t *pool,
                                    buckets_worker_stats_t *stats);

/**
 * Free worker pool
 * 
 * @param pool Worker pool
 */
void buckets_worker_pool_free(buckets_worker_pool_t *pool);

/* ===================================================================
 * Migration Job API
 * ===================================================================*/

/**
 * Create migration job
 * 
 * @param source_gen Source topology generation
 * @param target_gen Target topology generation  
 * @param old_topology Source topology
 * @param new_topology Target topology
 * @param disk_paths Array of disk paths
 * @param disk_count Number of disks
 * @return Job handle or NULL on error
 */
buckets_migration_job_t* buckets_migration_job_create(i64 source_gen, 
                                                       i64 target_gen,
                                                       buckets_cluster_topology_t *old_topology,
                                                       buckets_cluster_topology_t *new_topology,
                                                       char **disk_paths,
                                                       int disk_count);

/**
 * Start migration job
 * 
 * Transitions from IDLE to SCANNING state.
 * 
 * @param job Job handle
 * @return BUCKETS_OK on success
 */
int buckets_migration_job_start(buckets_migration_job_t *job);

/**
 * Pause migration job
 * 
 * Transitions from MIGRATING to PAUSED state.
 * 
 * @param job Job handle
 * @return BUCKETS_OK on success
 */
int buckets_migration_job_pause(buckets_migration_job_t *job);

/**
 * Resume migration job
 * 
 * Transitions from PAUSED to MIGRATING state.
 * 
 * @param job Job handle
 * @return BUCKETS_OK on success
 */
int buckets_migration_job_resume(buckets_migration_job_t *job);

/**
 * Stop migration job
 * 
 * Gracefully stops the job and transitions to FAILED state.
 * 
 * @param job Job handle
 * @return BUCKETS_OK on success
 */
int buckets_migration_job_stop(buckets_migration_job_t *job);

/**
 * Wait for job completion
 * 
 * Blocks until job reaches COMPLETED or FAILED state.
 * 
 * @param job Job handle
 * @return BUCKETS_OK on success
 */
int buckets_migration_job_wait(buckets_migration_job_t *job);

/**
 * Get job state
 * 
 * @param job Job handle
 * @return Current state
 */
buckets_migration_state_t buckets_migration_job_get_state(buckets_migration_job_t *job);

/**
 * Get job progress
 * 
 * @param job Job handle
 * @param total Output: total objects
 * @param completed Output: completed objects
 * @param failed Output: failed objects
 * @param percent Output: percentage complete (0-100)
 * @param eta Output: estimated time to completion (seconds)
 * @return BUCKETS_OK on success
 */
int buckets_migration_job_get_progress(buckets_migration_job_t *job,
                                         i64 *total,
                                         i64 *completed,
                                         i64 *failed,
                                         double *percent,
                                         i64 *eta);

/**
 * Set event callback
 * 
 * @param job Job handle
 * @param callback Callback function
 * @param user_data User data passed to callback
 * @return BUCKETS_OK on success
 */
int buckets_migration_job_set_callback(buckets_migration_job_t *job,
                                         buckets_migration_event_callback_t callback,
                                         void *user_data);

/**
 * Save job state to disk
 * 
 * @param job Job handle
 * @param path File path for checkpoint
 * @return BUCKETS_OK on success
 */
int buckets_migration_job_save(buckets_migration_job_t *job, const char *path);

/**
 * Load job state from disk
 * 
 * @param path File path for checkpoint
 * @return Job handle or NULL on error
 */
buckets_migration_job_t* buckets_migration_job_load(const char *path);

/**
 * Cleanup job
 * 
 * @param job Job handle
 */
void buckets_migration_job_cleanup(buckets_migration_job_t *job);

/* ===================================================================
 * Throttle API (Week 28)
 * ===================================================================*/

/**
 * Throttle structure
 * 
 * Token bucket algorithm for bandwidth limiting.
 */
typedef struct {
    i64 tokens;                     // Available tokens (bytes)
    i64 rate_bytes_per_sec;         // Refill rate (bytes/sec)
    i64 burst_bytes;                // Maximum burst size (bytes)
    i64 last_refill_us;             // Last refill time (microseconds)
    bool enabled;                   // Throttling enabled flag
    pthread_mutex_t lock;           // Thread safety
} buckets_throttle_t;

/**
 * Initialize throttle
 * 
 * @param throttle Throttle structure
 * @param rate_bytes_per_sec Rate limit in bytes per second (0 = unlimited)
 * @param burst_bytes Maximum burst size in bytes
 * @return BUCKETS_OK on success
 */
int buckets_throttle_init(buckets_throttle_t *throttle,
                           i64 rate_bytes_per_sec,
                           i64 burst_bytes);

/**
 * Cleanup throttle
 * 
 * @param throttle Throttle structure
 */
void buckets_throttle_cleanup(buckets_throttle_t *throttle);

/**
 * Wait for tokens to become available
 * 
 * Blocks until sufficient tokens are available, then consumes them.
 * 
 * @param throttle Throttle handle
 * @param bytes Number of bytes to consume
 * @return BUCKETS_OK on success
 */
int buckets_throttle_wait(buckets_throttle_t *throttle, i64 bytes);

/**
 * Set throttle rate (can be changed dynamically)
 * 
 * @param throttle Throttle handle
 * @param rate_bytes_per_sec New rate limit (0 = unlimited)
 * @return BUCKETS_OK on success
 */
int buckets_throttle_set_rate(buckets_throttle_t *throttle, i64 rate_bytes_per_sec);

/**
 * Get current throttle rate
 * 
 * @param throttle Throttle handle
 * @return Current rate in bytes per second
 */
i64 buckets_throttle_get_rate(buckets_throttle_t *throttle);

/**
 * Enable/disable throttling
 * 
 * @param throttle Throttle handle
 * @param enabled Enable flag
 * @return BUCKETS_OK on success
 */
int buckets_throttle_set_enabled(buckets_throttle_t *throttle, bool enabled);

/**
 * Check if throttling is enabled
 * 
 * @param throttle Throttle handle
 * @return true if enabled, false otherwise
 */
bool buckets_throttle_is_enabled(buckets_throttle_t *throttle);

/**
 * Get throttle statistics
 * 
 * @param throttle Throttle handle
 * @param current_tokens Output: current token count
 * @param rate_bytes_per_sec Output: current rate
 * @param enabled Output: enabled flag
 * @return BUCKETS_OK on success
 */
int buckets_throttle_get_stats(buckets_throttle_t *throttle,
                                i64 *current_tokens,
                                i64 *rate_bytes_per_sec,
                                bool *enabled);

/**
 * Create throttle with default settings
 * 
 * Default: 100 MB/s rate, 10 MB burst
 * 
 * @return Throttle handle or NULL on error
 */
buckets_throttle_t* buckets_throttle_create_default(void);

/**
 * Create throttle with custom settings
 * 
 * @param rate_mbps Rate limit in MB/s (0 = unlimited)
 * @param burst_mb Burst size in MB
 * @return Throttle handle or NULL on error
 */
buckets_throttle_t* buckets_throttle_create(i64 rate_mbps, i64 burst_mb);

/**
 * Free throttle
 * 
 * @param throttle Throttle handle
 */
void buckets_throttle_free(buckets_throttle_t *throttle);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_MIGRATION_H */
