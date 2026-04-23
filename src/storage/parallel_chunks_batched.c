/**
 * Batched Parallel Chunk Operations
 * 
 * Optimized version of parallel chunk writes that groups chunks by destination
 * node and sends them in batches to reduce network round-trips.
 * 
 * Example: For 12 chunks distributed across 3 nodes (4 chunks each):
 *   WITHOUT batching: 12 individual HTTP requests
 *   WITH batching:    3 HTTP requests (4 chunks each)
 *   
 * Expected improvement: 30-50% throughput increase for distributed writes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_placement.h"

/* Maximum chunks per batch */
#define MAX_BATCH_SIZE 16

/**
 * Node batch - groups chunks destined for the same node
 */
typedef struct {
    char node_endpoint[256];          /* Node endpoint */
    buckets_batch_chunk_t chunks[MAX_BATCH_SIZE];  /* Chunks for this node */
    size_t chunk_count;               /* Number of chunks */
    bool is_local;                    /* True if local writes */
    int result;                       /* Batch write result */
    pthread_t thread;                 /* Thread for this batch */
} node_batch_t;

/**
 * Worker thread for batched chunk write
 */
static void* batch_write_worker(void *arg)
{
    node_batch_t *batch = (node_batch_t*)arg;
    
    if (batch->is_local) {
        /* Local writes - process each chunk sequentially in this thread */
        batch->result = 0;
        
        for (size_t i = 0; i < batch->chunk_count; i++) {
            buckets_batch_chunk_t *chunk = &batch->chunks[i];
            
            extern int buckets_write_chunk(const char *disk_path, const char *object_path,
                                           u32 chunk_index, const void *data, size_t size);
            
            /* Compute object path from bucket/object */
            char object_path[1536];
            extern void buckets_compute_object_path(const char *bucket, const char *object,
                                                    char *object_path, size_t path_len);
            buckets_compute_object_path(chunk->bucket, chunk->object, 
                                       object_path, sizeof(object_path));
            
            int ret = buckets_write_chunk(chunk->disk_path, object_path,
                                         chunk->chunk_index, chunk->chunk_data, 
                                         chunk->chunk_size);
            if (ret != 0) {
                buckets_error("[BATCH_LOCAL] Failed to write chunk %u locally", chunk->chunk_index);
                batch->result = -1;
            } else {
                buckets_debug("[BATCH_LOCAL] Wrote chunk %u locally", chunk->chunk_index);
            }
        }
    } else {
        /* Remote batch write */
        extern int buckets_binary_batch_write_chunks(const char *peer_endpoint,
                                                      const buckets_batch_chunk_t *chunks,
                                                      size_t chunk_count);
        
        batch->result = buckets_binary_batch_write_chunks(batch->node_endpoint,
                                                          batch->chunks,
                                                          batch->chunk_count);
        
        if (batch->result == 0) {
            buckets_debug("[BATCH_REMOTE] Wrote %zu chunks to %s", 
                         batch->chunk_count, batch->node_endpoint);
        } else {
            buckets_error("[BATCH_REMOTE] Failed to write %zu chunks to %s",
                         batch->chunk_count, batch->node_endpoint);
        }
    }
    
    return NULL;
}

/**
 * Write multiple chunks in parallel with automatic batching by node
 * 
 * This is an optimized version of buckets_parallel_write_chunks that:
 * 1. Groups chunks by destination node endpoint
 * 2. Sends each group as a single batch HTTP request
 * 3. Reduces network round-trips by ~4x for typical erasure coding (4 nodes)
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param object_path Hashed object path
 * @param placement Placement result with disk endpoints
 * @param chunk_data_array Array of chunk data pointers
 * @param chunk_size Chunk size (uniform for all chunks)
 * @param num_chunks Total number of chunks (K+M)
 * @return 0 on success, -1 on error
 */
int buckets_batched_parallel_write_chunks(const char *bucket,
                                          const char *object,
                                          const char *object_path,
                                          buckets_placement_result_t *placement,
                                          const void **chunk_data_array,
                                          size_t chunk_size,
                                          u32 num_chunks)
{
    if (!bucket || !object || !object_path || !placement || !chunk_data_array) {
        buckets_error("Invalid parameters for batched parallel write");
        return -1;
    }
    
    if (num_chunks > 32) {
        buckets_error("Too many chunks: %u (max 32)", num_chunks);
        return -1;
    }
    
    /* Check if we have endpoints for distributed mode */
    bool has_endpoints = (placement->disk_endpoints && 
                         placement->disk_endpoints[0] && 
                         placement->disk_endpoints[0][0] != '\0');
    
    if (!has_endpoints) {
        /* No endpoints, fall back to non-batched parallel write */
        extern int buckets_parallel_write_chunks(const char *bucket,
                                                 const char *object,
                                                 const char *object_path,
                                                 buckets_placement_result_t *placement,
                                                 const void **chunk_data_array,
                                                 size_t chunk_size,
                                                 u32 num_chunks);
        return buckets_parallel_write_chunks(bucket, object, object_path, placement,
                                            chunk_data_array, chunk_size, num_chunks);
    }
    
    buckets_info("[BATCHED_WRITE] Starting batched write: %u chunks for %s/%s", 
                 num_chunks, bucket, object);
    
    /* Group chunks by node endpoint */
    node_batch_t batches[32];  /* Max 32 different nodes (overkill but safe) */
    size_t batch_count = 0;
    
    for (size_t i = 0; i < 32; i++) {
        batches[i].chunk_count = 0;
        batches[i].is_local = false;
        batches[i].result = -1;
        batches[i].thread = 0;
        batches[i].node_endpoint[0] = '\0';
    }
    
    /* Classify each chunk into a batch */
    for (u32 i = 0; i < num_chunks; i++) {
        char node_endpoint[256] = {0};
        bool is_local = false;
        
        /* Determine node endpoint and locality */
        if (placement->disk_endpoints[i] == NULL) {
            buckets_error("NULL disk_endpoint at index %u", i);
            return -1;
        }
        
        extern bool buckets_distributed_is_local_disk(const char *disk_endpoint);
        is_local = buckets_distributed_is_local_disk(placement->disk_endpoints[i]);
        
        if (!is_local) {
            extern int buckets_distributed_extract_node_endpoint(const char *disk_endpoint,
                                                                 char *node_endpoint, size_t size);
            if (buckets_distributed_extract_node_endpoint(placement->disk_endpoints[i],
                                                         node_endpoint,
                                                         sizeof(node_endpoint)) != 0) {
                buckets_error("Failed to extract node endpoint from: %s", placement->disk_endpoints[i]);
                return -1;
            }
        } else {
            snprintf(node_endpoint, sizeof(node_endpoint), "local");
        }
        
        /* Find or create batch for this node */
        node_batch_t *target_batch = NULL;
        for (size_t b = 0; b < batch_count; b++) {
            if (strcmp(batches[b].node_endpoint, node_endpoint) == 0) {
                target_batch = &batches[b];
                break;
            }
        }
        
        if (!target_batch) {
            /* Create new batch */
            if (batch_count >= 32) {
                buckets_error("Too many distinct nodes (>32)");
                return -1;
            }
            target_batch = &batches[batch_count++];
            snprintf(target_batch->node_endpoint, sizeof(target_batch->node_endpoint), "%s", node_endpoint);
            target_batch->is_local = is_local;
            target_batch->chunk_count = 0;
        }
        
        /* Add chunk to batch */
        if (target_batch->chunk_count >= MAX_BATCH_SIZE) {
            buckets_error("Batch size exceeded for node %s", node_endpoint);
            return -1;
        }
        
        buckets_batch_chunk_t *chunk = &target_batch->chunks[target_batch->chunk_count++];
        chunk->chunk_index = i + 1;
        chunk->chunk_data = chunk_data_array[i];
        chunk->chunk_size = chunk_size;
        strncpy(chunk->bucket, bucket, sizeof(chunk->bucket) - 1);
        strncpy(chunk->object, object, sizeof(chunk->object) - 1);
        
        if (placement->disk_paths[i] == NULL) {
            buckets_error("NULL disk_path at index %u", i);
            return -1;
        }
        strncpy(chunk->disk_path, placement->disk_paths[i], sizeof(chunk->disk_path) - 1);
    }
    
    buckets_info("[BATCHED_WRITE] Grouped %u chunks into %zu batches", num_chunks, batch_count);
    
    /* Log batch distribution */
    for (size_t b = 0; b < batch_count; b++) {
        buckets_info("[BATCHED_WRITE] Batch %zu: %zu chunks → %s %s",
                     b, batches[b].chunk_count, 
                     batches[b].is_local ? "LOCAL" : "REMOTE",
                     batches[b].node_endpoint);
    }
    
    /* Launch batch write threads */
    for (size_t b = 0; b < batch_count; b++) {
        int ret = pthread_create(&batches[b].thread, NULL, batch_write_worker, &batches[b]);
        if (ret != 0) {
            buckets_error("Failed to create batch write thread %zu: %d", b, ret);
            batches[b].result = -1;
        }
    }
    
    /* Wait for all batches to complete */
    int failed_batches = 0;
    int failed_chunks = 0;
    
    for (size_t b = 0; b < batch_count; b++) {
        if (batches[b].thread != 0) {
            pthread_join(batches[b].thread, NULL);
        }
        
        if (batches[b].result != 0) {
            buckets_error("[BATCHED_WRITE] Batch %zu failed (%zu chunks to %s)",
                         b, batches[b].chunk_count, batches[b].node_endpoint);
            failed_batches++;
            failed_chunks += batches[b].chunk_count;
        } else {
            buckets_debug("[BATCHED_WRITE] Batch %zu succeeded (%zu chunks)",
                         b, batches[b].chunk_count);
        }
    }
    
    if (failed_batches > 0) {
        buckets_warn("[BATCHED_WRITE] %d batches failed (%d chunks total)", 
                     failed_batches, failed_chunks);
        return -1;
    }
    
    buckets_info("[BATCHED_WRITE] SUCCESS: All %zu batches completed (%u chunks total)",
                 batch_count, num_chunks);
    
    return 0;
}
