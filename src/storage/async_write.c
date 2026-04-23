/**
 * Async Write Completion Implementation
 * 
 * Background worker threads that complete chunk writes after client ACK.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>

#include "buckets.h"
#include "buckets_async_write.h"
#include "buckets_storage.h"
#include "buckets_profile.h"

/* Global async write queue */
static async_write_queue_t *g_async_queue = NULL;
static uint64_t g_next_job_id = 1;
static pthread_mutex_t g_job_id_lock = PTHREAD_MUTEX_INITIALIZER;

/* Get current time in microseconds */
static uint64_t get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* Allocate job ID */
static uint64_t alloc_job_id(void)
{
    pthread_mutex_lock(&g_job_id_lock);
    uint64_t id = g_next_job_id++;
    pthread_mutex_unlock(&g_job_id_lock);
    return id;
}

/* Worker thread function */
static void* async_write_worker(void *arg)
{
    async_write_queue_t *queue = (async_write_queue_t*)arg;
    
    buckets_info("[ASYNC_WRITE] Worker thread started");
    
    while (1) {
        async_write_job_t *job = NULL;
        
        /* Dequeue job */
        pthread_mutex_lock(&queue->lock);
        
        while (queue->head == NULL && !queue->shutdown) {
            pthread_cond_wait(&queue->cond, &queue->lock);
        }
        
        if (queue->shutdown && queue->head == NULL) {
            pthread_mutex_unlock(&queue->lock);
            break;
        }
        
        job = queue->head;
        queue->head = job->next;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
        queue->count--;
        
        pthread_mutex_unlock(&queue->lock);
        
        if (!job) continue;
        
        /* Process job */
        PROFILE_START(async_write_job);
        
        pthread_mutex_lock(&job->lock);
        job->state = ASYNC_WRITE_IN_PROGRESS;
        pthread_mutex_unlock(&job->lock);
        
        buckets_info("[ASYNC_WRITE] Processing job %lu: %s/%s (%u chunks)",
                     job->job_id, job->bucket, job->object, job->num_chunks);
        
        /* Write chunks using batched parallel writes */
        extern int buckets_batched_parallel_write_chunks(const char *bucket,
                                                         const char *object,
                                                         const char *object_path,
                                                         buckets_placement_result_t *placement,
                                                         const void **chunk_data_array,
                                                         size_t chunk_size,
                                                         u32 num_chunks);
        
        int result = buckets_batched_parallel_write_chunks(job->bucket, job->object,
                                                           job->object_path, job->placement,
                                                           (const void**)job->chunk_data,
                                                           job->chunk_size, job->num_chunks);
        
        if (result == 0) {
            /* Write metadata */
            extern int buckets_parallel_write_metadata(const char *bucket,
                                                       const char *object,
                                                       const char *object_path,
                                                       buckets_placement_result_t *placement,
                                                       char **disk_paths,
                                                       const buckets_xl_meta_t *base_meta,
                                                       u32 num_disks,
                                                       bool has_endpoints);
            
            bool has_endpoints = (job->placement && job->placement->disk_endpoints &&
                                 job->placement->disk_endpoints[0] &&
                                 job->placement->disk_endpoints[0][0] != '\0');
            
            result = buckets_parallel_write_metadata(job->bucket, job->object, job->object_path,
                                                     job->placement, job->placement->disk_paths,
                                                     &job->meta, job->num_chunks, has_endpoints);
        }
        
        PROFILE_END(async_write_job, "Async write complete: result=%d", result);
        
        /* Update job state */
        pthread_mutex_lock(&job->lock);
        job->result = result;
        job->state = (result == 0) ? ASYNC_WRITE_COMPLETE : ASYNC_WRITE_FAILED;
        job->complete_time_us = get_time_us();
        pthread_cond_broadcast(&job->cond);
        pthread_mutex_unlock(&job->lock);
        
        /* Update stats */
        pthread_mutex_lock(&queue->lock);
        if (result == 0) {
            queue->total_completed++;
            buckets_info("[ASYNC_WRITE] Job %lu completed successfully (total: %lu)",
                        job->job_id, queue->total_completed);
        } else {
            queue->total_failed++;
            buckets_error("[ASYNC_WRITE] Job %lu failed (total failures: %lu)",
                         job->job_id, queue->total_failed);
        }
        pthread_mutex_unlock(&queue->lock);
        
        /* Cleanup job resources */
        for (u32 i = 0; i < job->num_chunks; i++) {
            buckets_free(job->chunk_data[i]);
        }
        buckets_free(job->chunk_data);
        
        if (job->placement) {
            buckets_placement_free_result(job->placement);
        }
        
        buckets_xl_meta_free(&job->meta);
        
        pthread_mutex_destroy(&job->lock);
        pthread_cond_destroy(&job->cond);
        buckets_free(job);
    }
    
    buckets_info("[ASYNC_WRITE] Worker thread exiting");
    return NULL;
}

int buckets_async_write_init(size_t num_workers)
{
    if (g_async_queue) {
        buckets_warn("Async write system already initialized");
        return BUCKETS_OK;
    }
    
    if (num_workers == 0) {
        num_workers = 4;  /* Default */
    }
    
    g_async_queue = buckets_calloc(1, sizeof(*g_async_queue));
    if (!g_async_queue) {
        return BUCKETS_ERR_NOMEM;
    }
    
    pthread_mutex_init(&g_async_queue->lock, NULL);
    pthread_cond_init(&g_async_queue->cond, NULL);
    g_async_queue->num_workers = num_workers;
    
    /* Start worker threads */
    g_async_queue->workers = buckets_calloc(num_workers, sizeof(pthread_t));
    if (!g_async_queue->workers) {
        buckets_free(g_async_queue);
        g_async_queue = NULL;
        return BUCKETS_ERR_NOMEM;
    }
    
    for (size_t i = 0; i < num_workers; i++) {
        if (pthread_create(&g_async_queue->workers[i], NULL,
                          async_write_worker, g_async_queue) != 0) {
            buckets_error("Failed to create async write worker %zu", i);
            /* Cleanup already created threads */
            g_async_queue->shutdown = true;
            pthread_cond_broadcast(&g_async_queue->cond);
            for (size_t j = 0; j < i; j++) {
                pthread_join(g_async_queue->workers[j], NULL);
            }
            buckets_free(g_async_queue->workers);
            buckets_free(g_async_queue);
            g_async_queue = NULL;
            return BUCKETS_ERR_INIT;
        }
    }
    
    buckets_info("[ASYNC_WRITE] Initialized with %zu worker threads", num_workers);
    return BUCKETS_OK;
}

void buckets_async_write_shutdown(void)
{
    if (!g_async_queue) {
        return;
    }
    
    buckets_info("[ASYNC_WRITE] Shutting down...");
    
    /* Signal shutdown */
    pthread_mutex_lock(&g_async_queue->lock);
    g_async_queue->shutdown = true;
    pthread_cond_broadcast(&g_async_queue->cond);
    pthread_mutex_unlock(&g_async_queue->lock);
    
    /* Wait for workers */
    for (size_t i = 0; i < g_async_queue->num_workers; i++) {
        pthread_join(g_async_queue->workers[i], NULL);
    }
    
    buckets_free(g_async_queue->workers);
    
    /* Free remaining jobs */
    async_write_job_t *job = g_async_queue->head;
    while (job) {
        async_write_job_t *next = job->next;
        
        for (u32 i = 0; i < job->num_chunks; i++) {
            buckets_free(job->chunk_data[i]);
        }
        buckets_free(job->chunk_data);
        
        if (job->placement) {
            buckets_placement_free_result(job->placement);
        }
        
        buckets_xl_meta_free(&job->meta);
        pthread_mutex_destroy(&job->lock);
        pthread_cond_destroy(&job->cond);
        buckets_free(job);
        
        job = next;
    }
    
    pthread_mutex_destroy(&g_async_queue->lock);
    pthread_cond_destroy(&g_async_queue->cond);
    buckets_free(g_async_queue);
    g_async_queue = NULL;
    
    buckets_info("[ASYNC_WRITE] Shutdown complete");
}

int buckets_async_write_queue(const char *bucket,
                               const char *object,
                               const char *object_path,
                               buckets_placement_result_t *placement,
                               void **chunk_data,
                               size_t chunk_size,
                               uint32_t num_chunks,
                               const buckets_xl_meta_t *meta,
                               uint64_t *job_id_out)
{
    if (!g_async_queue) {
        buckets_error("Async write system not initialized");
        return BUCKETS_ERR_INIT;
    }
    
    /* Check queue depth */
    pthread_mutex_lock(&g_async_queue->lock);
    if (g_async_queue->count >= MAX_ASYNC_WRITES) {
        pthread_mutex_unlock(&g_async_queue->lock);
        buckets_error("Async write queue full (%d jobs)", MAX_ASYNC_WRITES);
        return BUCKETS_ERR_NOMEM;
    }
    pthread_mutex_unlock(&g_async_queue->lock);
    
    /* Create job */
    async_write_job_t *job = buckets_calloc(1, sizeof(*job));
    if (!job) {
        return BUCKETS_ERR_NOMEM;
    }
    
    job->job_id = alloc_job_id();
    snprintf(job->bucket, sizeof(job->bucket), "%s", bucket);
    snprintf(job->object, sizeof(job->object), "%s", object);
    snprintf(job->object_path, sizeof(job->object_path), "%s", object_path);
    
    job->chunk_data = chunk_data;
    job->chunk_size = chunk_size;
    job->num_chunks = num_chunks;
    job->placement = placement;
    
    /* Copy metadata */
    memcpy(&job->meta, meta, sizeof(*meta));
    
    /* Deep copy metadata strings */
    if (meta->meta.content_type) {
        job->meta.meta.content_type = buckets_strdup(meta->meta.content_type);
    }
    if (meta->meta.user_count > 0) {
        job->meta.meta.user_keys = buckets_malloc(meta->meta.user_count * sizeof(char*));
        job->meta.meta.user_values = buckets_malloc(meta->meta.user_count * sizeof(char*));
        for (u32 i = 0; i < meta->meta.user_count; i++) {
            job->meta.meta.user_keys[i] = buckets_strdup(meta->meta.user_keys[i]);
            job->meta.meta.user_values[i] = buckets_strdup(meta->meta.user_values[i]);
        }
    }
    if (meta->erasure.distribution) {
        size_t dist_size = (meta->erasure.data + meta->erasure.parity) * sizeof(u32);
        job->meta.erasure.distribution = buckets_malloc(dist_size);
        memcpy(job->meta.erasure.distribution, meta->erasure.distribution, dist_size);
    }
    if (meta->erasure.checksums) {
        size_t cs_size = (meta->erasure.data + meta->erasure.parity) * sizeof(buckets_checksum_t);
        job->meta.erasure.checksums = buckets_malloc(cs_size);
        memcpy(job->meta.erasure.checksums, meta->erasure.checksums, cs_size);
    }
    
    job->state = ASYNC_WRITE_PENDING;
    job->queued_time_us = get_time_us();
    pthread_mutex_init(&job->lock, NULL);
    pthread_cond_init(&job->cond, NULL);
    
    /* Enqueue */
    pthread_mutex_lock(&g_async_queue->lock);
    
    if (g_async_queue->tail) {
        g_async_queue->tail->next = job;
    } else {
        g_async_queue->head = job;
    }
    g_async_queue->tail = job;
    g_async_queue->count++;
    g_async_queue->total_queued++;
    
    pthread_cond_signal(&g_async_queue->cond);
    pthread_mutex_unlock(&g_async_queue->lock);
    
    buckets_info("[ASYNC_WRITE] Queued job %lu: %s/%s (%u chunks, queue_depth=%zu)",
                 job->job_id, bucket, object, num_chunks, g_async_queue->count);
    
    if (job_id_out) {
        *job_id_out = job->job_id;
    }
    
    return BUCKETS_OK;
}

void buckets_async_write_stats(uint64_t *queued, uint64_t *completed,
                                uint64_t *failed, size_t *queue_depth)
{
    if (!g_async_queue) {
        if (queued) *queued = 0;
        if (completed) *completed = 0;
        if (failed) *failed = 0;
        if (queue_depth) *queue_depth = 0;
        return;
    }
    
    pthread_mutex_lock(&g_async_queue->lock);
    if (queued) *queued = g_async_queue->total_queued;
    if (completed) *completed = g_async_queue->total_completed;
    if (failed) *failed = g_async_queue->total_failed;
    if (queue_depth) *queue_depth = g_async_queue->count;
    pthread_mutex_unlock(&g_async_queue->lock);
}
