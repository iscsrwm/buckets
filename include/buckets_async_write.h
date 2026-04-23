/**
 * Async Write Completion System
 * 
 * Enables pipelined ACK responses by queueing writes to complete in background.
 * This allows sending HTTP 200 to client immediately after erasure encoding,
 * while chunk writes complete asynchronously.
 */

#ifndef BUCKETS_ASYNC_WRITE_H
#define BUCKETS_ASYNC_WRITE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_placement.h"

/* Maximum pending async writes */
#define MAX_ASYNC_WRITES 1024

/* Async write states */
typedef enum {
    ASYNC_WRITE_PENDING,      /* Queued, not yet started */
    ASYNC_WRITE_IN_PROGRESS,  /* Currently writing */
    ASYNC_WRITE_COMPLETE,     /* Successfully completed */
    ASYNC_WRITE_FAILED        /* Failed */
} async_write_state_t;

/* Async write job */
typedef struct async_write_job {
    /* Identification */
    uint64_t job_id;
    char bucket[256];
    char object[1024];
    char object_path[2048];
    
    /* Write data */
    void **chunk_data;              /* Array of chunk pointers (K+M) */
    size_t chunk_size;              /* Size of each chunk */
    uint32_t num_chunks;            /* Total chunks (K+M) */
    buckets_placement_result_t *placement;  /* Placement info */
    buckets_xl_meta_t meta;         /* Metadata to write */
    
    /* State tracking */
    async_write_state_t state;
    int result;                     /* 0 on success, -1 on failure */
    uint64_t queued_time_us;        /* When queued */
    uint64_t complete_time_us;      /* When completed */
    
    /* Synchronization */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    
    /* Linked list */
    struct async_write_job *next;
} async_write_job_t;

/* Async write queue */
typedef struct {
    async_write_job_t *head;
    async_write_job_t *tail;
    size_t count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool shutdown;
    
    /* Worker threads */
    pthread_t *workers;
    size_t num_workers;
    
    /* Stats */
    uint64_t total_queued;
    uint64_t total_completed;
    uint64_t total_failed;
} async_write_queue_t;

/**
 * Initialize async write system
 * 
 * @param num_workers Number of background worker threads
 * @return BUCKETS_OK on success
 */
int buckets_async_write_init(size_t num_workers);

/**
 * Shutdown async write system
 */
void buckets_async_write_shutdown(void);

/**
 * Queue async write job
 * 
 * Ownership of chunk_data and placement is transferred to the async system.
 * They will be freed when the job completes.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param object_path Computed object path
 * @param placement Placement result (ownership transferred)
 * @param chunk_data Array of chunk pointers (ownership transferred)
 * @param chunk_size Size of each chunk
 * @param num_chunks Total number of chunks
 * @param meta Metadata to write (copied)
 * @param job_id_out Output: Job ID for tracking
 * @return BUCKETS_OK on success
 */
int buckets_async_write_queue(const char *bucket,
                               const char *object,
                               const char *object_path,
                               buckets_placement_result_t *placement,
                               void **chunk_data,
                               size_t chunk_size,
                               uint32_t num_chunks,
                               const buckets_xl_meta_t *meta,
                               uint64_t *job_id_out);

/**
 * Check async write status
 * 
 * @param job_id Job ID from buckets_async_write_queue
 * @param state_out Output: Current state
 * @param result_out Output: Result (0 = success, -1 = failure)
 * @return BUCKETS_OK if job exists
 */
int buckets_async_write_status(uint64_t job_id,
                                async_write_state_t *state_out,
                                int *result_out);

/**
 * Wait for async write to complete
 * 
 * @param job_id Job ID from buckets_async_write_queue
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @return BUCKETS_OK on success
 */
int buckets_async_write_wait(uint64_t job_id, uint32_t timeout_ms);

/**
 * Get async write statistics
 */
void buckets_async_write_stats(uint64_t *queued, uint64_t *completed, 
                                uint64_t *failed, size_t *queue_depth);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_ASYNC_WRITE_H */
