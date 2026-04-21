/**
 * Async Replication Implementation
 * 
 * Lock-free queue with background worker threads for async replication.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "buckets.h"
#include "async_replication.h"

/* Maximum pending replications before blocking */
#define MAX_QUEUE_SIZE 10000

/* Replication work item */
typedef struct replication_work {
    char *bucket;
    char *object;
    char *object_path;
    buckets_xl_meta_t meta;
    buckets_placement_result_t *placement;
    struct replication_work *next;
} replication_work_t;

/* Replication queue */
typedef struct {
    replication_work_t *head;
    replication_work_t *tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    size_t count;
    bool shutdown;
    
    /* Stats */
    size_t completed;
    size_t failed;
} replication_queue_t;

/* Worker thread pool */
typedef struct {
    pthread_t *threads;
    int num_workers;
    replication_queue_t queue;
} replication_pool_t;

static replication_pool_t g_replication_pool = {0};

/* ===================================================================
 * Queue Operations
 * ===================================================================*/

static replication_work_t* dequeue_work(replication_queue_t *queue)
{
    pthread_mutex_lock(&queue->lock);
    
    while (queue->head == NULL && !queue->shutdown) {
        pthread_cond_wait(&queue->cond, &queue->lock);
    }
    
    if (queue->shutdown && queue->head == NULL) {
        pthread_mutex_unlock(&queue->lock);
        return NULL;
    }
    
    replication_work_t *work = queue->head;
    queue->head = work->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    queue->count--;
    
    pthread_mutex_unlock(&queue->lock);
    return work;
}

static int enqueue_work(replication_queue_t *queue, replication_work_t *work)
{
    pthread_mutex_lock(&queue->lock);
    
    if (queue->count >= MAX_QUEUE_SIZE) {
        pthread_mutex_unlock(&queue->lock);
        buckets_warn("Replication queue full (%zu items), dropping replication", 
                    queue->count);
        return -1;
    }
    
    work->next = NULL;
    
    if (queue->tail) {
        queue->tail->next = work;
    } else {
        queue->head = work;
    }
    queue->tail = work;
    queue->count++;
    
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
    
    return 0;
}

/* ===================================================================
 * Worker Thread
 * ===================================================================*/

static void* replication_worker(void *arg)
{
    replication_queue_t *queue = (replication_queue_t*)arg;
    
    buckets_info("Replication worker thread started");
    
    while (1) {
        replication_work_t *work = dequeue_work(queue);
        if (!work) {
            break;  /* Shutdown */
        }
        
        /* Perform replication */
        extern int buckets_parallel_write_metadata(const char *bucket,
                                                   const char *object,
                                                   const char *object_path,
                                                   buckets_placement_result_t *placement,
                                                   char **disk_paths,
                                                   const buckets_xl_meta_t *base_meta,
                                                   u32 num_disks,
                                                   bool has_endpoints);
        
        bool has_endpoints = (work->placement && work->placement->disk_endpoints && 
                             work->placement->disk_endpoints[0] && 
                             work->placement->disk_endpoints[0][0] != '\0');
        
        int ret = buckets_parallel_write_metadata(work->bucket, work->object, 
                                                  work->object_path,
                                                  work->placement, 
                                                  work->placement->disk_paths,
                                                  &work->meta, 
                                                  work->placement->disk_count, 
                                                  has_endpoints);
        
        pthread_mutex_lock(&queue->lock);
        if (ret == 0) {
            queue->completed++;
            buckets_debug("Async replication succeeded: %s/%s (%zu completed)", 
                         work->bucket, work->object, queue->completed);
        } else {
            queue->failed++;
            buckets_warn("Async replication failed: %s/%s (%zu failed)", 
                        work->bucket, work->object, queue->failed);
        }
        pthread_mutex_unlock(&queue->lock);
        
        /* Cleanup work item */
        buckets_free(work->bucket);
        buckets_free(work->object);
        buckets_free(work->object_path);
        buckets_xl_meta_free(&work->meta);
        buckets_placement_free_result(work->placement);
        buckets_free(work);
    }
    
    buckets_info("Replication worker thread exiting");
    return NULL;
}

/* ===================================================================
 * Public API
 * ===================================================================*/

int async_replication_init(int num_workers)
{
    if (num_workers <= 0) {
        num_workers = 4;  /* Default */
    }
    
    memset(&g_replication_pool, 0, sizeof(g_replication_pool));
    
    pthread_mutex_init(&g_replication_pool.queue.lock, NULL);
    pthread_cond_init(&g_replication_pool.queue.cond, NULL);
    
    g_replication_pool.num_workers = num_workers;
    g_replication_pool.threads = buckets_calloc(num_workers, sizeof(pthread_t));
    
    if (!g_replication_pool.threads) {
        return -1;
    }
    
    /* Start worker threads */
    for (int i = 0; i < num_workers; i++) {
        int ret = pthread_create(&g_replication_pool.threads[i], NULL,
                                replication_worker, &g_replication_pool.queue);
        if (ret != 0) {
            buckets_error("Failed to create replication worker %d", i);
            /* Continue with fewer workers */
        }
    }
    
    buckets_info("Async replication initialized with %d workers", num_workers);
    return 0;
}

void async_replication_shutdown(void)
{
    replication_queue_t *queue = &g_replication_pool.queue;
    
    /* Signal shutdown */
    pthread_mutex_lock(&queue->lock);
    queue->shutdown = true;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
    
    /* Wait for workers to finish */
    for (int i = 0; i < g_replication_pool.num_workers; i++) {
        if (g_replication_pool.threads[i]) {
            pthread_join(g_replication_pool.threads[i], NULL);
        }
    }
    
    buckets_info("Async replication shutdown complete (completed=%zu, failed=%zu)",
                queue->completed, queue->failed);
    
    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->cond);
    buckets_free(g_replication_pool.threads);
}

int async_replication_queue(const char *bucket,
                            const char *object,
                            const char *object_path,
                            const buckets_xl_meta_t *meta,
                            buckets_placement_result_t *placement)
{
    if (!bucket || !object || !object_path || !meta || !placement) {
        return -1;
    }
    
    /* Create work item */
    replication_work_t *work = buckets_calloc(1, sizeof(replication_work_t));
    if (!work) {
        return -1;
    }
    
    work->bucket = buckets_strdup(bucket);
    work->object = buckets_strdup(object);
    work->object_path = buckets_strdup(object_path);
    
    /* Deep copy metadata */
    memcpy(&work->meta, meta, sizeof(buckets_xl_meta_t));
    
    /* Deep copy strings in metadata */
    if (meta->bucket) work->meta.bucket = buckets_strdup(meta->bucket);
    if (meta->object) work->meta.object = buckets_strdup(meta->object);
    if (meta->inline_data) work->meta.inline_data = buckets_strdup(meta->inline_data);
    if (meta->meta.content_type) work->meta.meta.content_type = buckets_strdup(meta->meta.content_type);
    
    /* Copy user metadata */
    if (meta->meta.user_count > 0) {
        work->meta.meta.user_keys = buckets_calloc(meta->meta.user_count, sizeof(char*));
        work->meta.meta.user_values = buckets_calloc(meta->meta.user_count, sizeof(char*));
        for (u32 i = 0; i < meta->meta.user_count; i++) {
            work->meta.meta.user_keys[i] = buckets_strdup(meta->meta.user_keys[i]);
            work->meta.meta.user_values[i] = buckets_strdup(meta->meta.user_values[i]);
        }
    }
    
    /* Placement will be freed by worker, so caller must not free it */
    work->placement = placement;
    
    /* Enqueue work */
    if (enqueue_work(&g_replication_pool.queue, work) != 0) {
        /* Queue full, cleanup */
        buckets_free(work->bucket);
        buckets_free(work->object);
        buckets_free(work->object_path);
        buckets_xl_meta_free(&work->meta);
        buckets_free(work);
        return -1;
    }
    
    return 0;
}

void async_replication_stats(size_t *pending_out,
                             size_t *completed_out,
                             size_t *failed_out)
{
    replication_queue_t *queue = &g_replication_pool.queue;
    
    pthread_mutex_lock(&queue->lock);
    if (pending_out) *pending_out = queue->count;
    if (completed_out) *completed_out = queue->completed;
    if (failed_out) *failed_out = queue->failed;
    pthread_mutex_unlock(&queue->lock);
}
