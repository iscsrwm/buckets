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

/**
 * Top-level migration job
 */
typedef struct {
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
    
    pthread_mutex_t lock;                   /* Thread safety */
} buckets_migration_job_t;

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
 * Worker pool state (opaque)
 */
typedef struct buckets_worker_pool buckets_worker_pool_t;

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
 * Migration Job API (Preview - will implement in Week 27)
 * ===================================================================*/

/**
 * Create migration job
 * 
 * @param source_gen Source topology generation
 * @param target_gen Target topology generation
 * @return Job handle or NULL on error
 */
buckets_migration_job_t* buckets_migration_job_create(i64 source_gen, i64 target_gen);

/**
 * Get job status
 * 
 * @param job Job handle
 * @return Current state
 */
buckets_migration_state_t buckets_migration_job_get_state(buckets_migration_job_t *job);

/**
 * Cleanup job
 * 
 * @param job Job handle
 */
void buckets_migration_job_cleanup(buckets_migration_job_t *job);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_MIGRATION_H */
