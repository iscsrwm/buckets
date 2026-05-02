/**
 * Group Commit for Batched fsync
 * 
 * Provides batched fsync functionality to reduce the number of expensive
 * disk sync operations. Instead of syncing after every write, accumulate
 * multiple writes and sync them together.
 * 
 * Features:
 * - Per-disk batching with independent buffers
 * - Hybrid batching: count-based OR time-based
 * - Thread-safe access with per-disk locks
 * - fdatasync support (faster than fsync for most workloads)
 * - Configurable durability levels
 */

#ifndef BUCKETS_GROUP_COMMIT_H
#define BUCKETS_GROUP_COMMIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Default configuration values */
#define BUCKETS_GC_DEFAULT_BATCH_SIZE 64
#define BUCKETS_GC_DEFAULT_BATCH_TIME_MS 5
#define BUCKETS_GC_MAX_OPEN_FILES 256

/**
 * Durability level for writes
 */
typedef enum {
    BUCKETS_DURABILITY_NONE = 0,      /* No fsync (page cache only) - 10x faster, lose data on crash */
    BUCKETS_DURABILITY_BATCHED = 1,   /* Batched fsync - 5x faster, lose <10ms of writes [DEFAULT] */
    BUCKETS_DURABILITY_IMMEDIATE = 2  /* Immediate fsync - Current behavior, slowest but safest */
} buckets_durability_level_t;

/**
 * Group commit configuration
 */
typedef struct {
    uint32_t batch_size;              /* Number of writes before fsync (default: 64) */
    uint32_t batch_time_ms;           /* Time window before fsync in ms (default: 10) */
    bool use_fdatasync;               /* Use fdatasync instead of fsync (default: true) */
    buckets_durability_level_t durability; /* Durability level (default: BATCHED) */
} buckets_group_commit_config_t;

/**
 * Opaque group commit context
 */
typedef struct buckets_group_commit_context buckets_group_commit_context_t;

/* ===================================================================
 * Initialization & Configuration
 * ===================================================================*/

/**
 * Initialize group commit system
 * 
 * @param config Configuration (NULL for defaults)
 * @return Context handle, or NULL on error
 */
buckets_group_commit_context_t* buckets_group_commit_init(const buckets_group_commit_config_t *config);

/**
 * Cleanup group commit system
 * 
 * Flushes all pending writes and frees resources.
 * 
 * @param ctx Context to cleanup
 */
void buckets_group_commit_cleanup(buckets_group_commit_context_t *ctx);

/**
 * Get current configuration
 * 
 * @param ctx Context
 * @return Configuration structure (read-only)
 */
const buckets_group_commit_config_t* buckets_group_commit_get_config(buckets_group_commit_context_t *ctx);

/* ===================================================================
 * Write Operations
 * ===================================================================*/

/**
 * Write with group commit
 * 
 * Writes data to file descriptor and adds to batch for future sync.
 * May trigger immediate sync if batch is full or time window expired.
 * 
 * @param ctx Group commit context
 * @param fd File descriptor
 * @param buf Data to write
 * @param count Number of bytes to write
 * @return Number of bytes written, or -1 on error
 */
ssize_t buckets_group_commit_write(buckets_group_commit_context_t *ctx,
                                   int fd,
                                   const void *buf,
                                   size_t count);

/**
 * Write with group commit at offset (pwrite)
 * 
 * @param ctx Group commit context
 * @param fd File descriptor
 * @param buf Data to write
 * @param count Number of bytes to write
 * @param offset File offset
 * @return Number of bytes written, or -1 on error
 */
ssize_t buckets_group_commit_pwrite(buckets_group_commit_context_t *ctx,
                                    int fd,
                                    const void *buf,
                                    size_t count,
                                    off_t offset);

/* ===================================================================
 * Sync Operations
 * ===================================================================*/

/**
 * Flush pending writes for a file descriptor
 * 
 * Forces immediate sync of all pending writes for the given fd.
 * 
 * @param ctx Group commit context
 * @param fd File descriptor to flush
 * @return 0 on success, -1 on error
 */
int buckets_group_commit_flush_fd(buckets_group_commit_context_t *ctx, int fd);

/**
 * Flush all pending writes
 * 
 * Forces immediate sync of all pending writes across all file descriptors.
 * 
 * @param ctx Group commit context
 * @return 0 on success, -1 on error
 */
int buckets_group_commit_flush_all(buckets_group_commit_context_t *ctx);

/* ===================================================================
 * Statistics & Monitoring
 * ===================================================================*/

/**
 * Group commit statistics
 */
typedef struct {
    uint64_t total_writes;            /* Total write operations */
    uint64_t total_syncs;             /* Total sync operations */
    uint64_t batched_syncs;           /* Syncs from batching */
    uint64_t immediate_syncs;         /* Immediate syncs (batch full/time expired) */
    uint64_t explicit_flushes;        /* Explicit flush calls */
    double avg_batch_size;            /* Average writes per sync */
    uint64_t bytes_written;           /* Total bytes written */
} buckets_group_commit_stats_t;

/**
 * Get group commit statistics
 * 
 * @param ctx Group commit context
 * @param stats Output statistics structure
 * @return 0 on success, -1 on error
 */
int buckets_group_commit_get_stats(buckets_group_commit_context_t *ctx,
                                   buckets_group_commit_stats_t *stats);

/**
 * Reset statistics counters
 * 
 * @param ctx Group commit context
 */
void buckets_group_commit_reset_stats(buckets_group_commit_context_t *ctx);

/* ===================================================================
 * Convenience Functions
 * ===================================================================*/

/**
 * Get default configuration
 * 
 * @return Default configuration structure
 */
buckets_group_commit_config_t buckets_group_commit_default_config(void);

/**
 * Create configuration with custom durability level
 * 
 * @param durability Durability level
 * @return Configuration structure
 */
buckets_group_commit_config_t buckets_group_commit_config_for_durability(
    buckets_durability_level_t durability);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_GROUP_COMMIT_H */
