/**
 * Async Replication System
 * 
 * Provides background replication of small objects after local write succeeds.
 * Allows fast responses while maintaining eventual consistency and redundancy.
 */

#ifndef BUCKETS_ASYNC_REPLICATION_H
#define BUCKETS_ASYNC_REPLICATION_H

#include <stddef.h>
#include <stdbool.h>
#include "buckets_storage.h"
#include "buckets_placement.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize async replication system
 * 
 * @param num_workers Number of background worker threads
 * @return 0 on success
 */
int async_replication_init(int num_workers);

/**
 * Shutdown async replication system
 * 
 * Waits for pending replications to complete.
 */
void async_replication_shutdown(void);

/**
 * Queue object for async replication
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param object_path Hashed object path
 * @param meta Object metadata to replicate
 * @param placement Placement info with target disks
 * @return 0 if queued successfully
 */
int async_replication_queue(const char *bucket,
                            const char *object,
                            const char *object_path,
                            const buckets_xl_meta_t *meta,
                            buckets_placement_result_t *placement);

/**
 * Get replication queue stats
 * 
 * @param pending_out Number of pending replications
 * @param completed_out Number of completed replications
 * @param failed_out Number of failed replications
 */
void async_replication_stats(size_t *pending_out,
                             size_t *completed_out,
                             size_t *failed_out);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_ASYNC_REPLICATION_H */
