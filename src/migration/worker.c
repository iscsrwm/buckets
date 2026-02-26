/**
 * Migration Worker Implementation
 * 
 * Parallel object migration with thread pool.
 * 
 * Architecture:
 * 1. Worker pool manages N threads (default 16)
 * 2. Task queue (producer-consumer with condition variables)
 * 3. Each worker:
 *    - Picks task from queue
 *    - Reads object from source set
 *    - Writes object to destination set
 *    - Updates registry atomically
 *    - Deletes from source
 *    - Tracks stats and errors
 * 4. Retry logic: 3 attempts with exponential backoff
 * 5. Graceful shutdown on completion or error
 */

#define _XOPEN_SOURCE 600  /* For usleep */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_migration.h"
#include "buckets_registry.h"
#include "buckets_storage.h"

/* ===================================================================
 * Constants
 * ===================================================================*/

#define DEFAULT_NUM_WORKERS 16
#define MAX_RETRY_ATTEMPTS 3
#define INITIAL_RETRY_DELAY_MS 100
#define MAX_RETRY_DELAY_MS 5000

/* ===================================================================
 * Task Queue
 * ===================================================================*/

/**
 * Thread-safe task queue
 */
typedef struct {
    buckets_migration_task_t *tasks;   /* Task array */
    int capacity;                       /* Queue capacity */
    int count;                          /* Current count */
    int head;                           /* Queue head (consumer) */
    int tail;                           /* Queue tail (producer) */
    
    pthread_mutex_t lock;               /* Queue lock */
    pthread_cond_t not_empty;           /* Signal: tasks available */
    pthread_cond_t not_full;            /* Signal: space available */
    
    bool shutdown;                      /* Shutdown requested? */
} task_queue_t;

/**
 * Initialize task queue
 */
static task_queue_t* task_queue_init(int capacity)
{
    task_queue_t *queue = buckets_calloc(1, sizeof(task_queue_t));
    if (!queue) {
        return NULL;
    }
    
    queue->tasks = buckets_calloc(capacity, sizeof(buckets_migration_task_t));
    if (!queue->tasks) {
        buckets_free(queue);
        return NULL;
    }
    
    queue->capacity = capacity;
    queue->count = 0;
    queue->head = 0;
    queue->tail = 0;
    queue->shutdown = false;
    
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
    
    return queue;
}

/**
 * Push task to queue (blocking if full)
 */
static int task_queue_push(task_queue_t *queue, buckets_migration_task_t *task)
{
    pthread_mutex_lock(&queue->lock);
    
    /* Wait for space */
    while (queue->count >= queue->capacity && !queue->shutdown) {
        pthread_cond_wait(&queue->not_full, &queue->lock);
    }
    
    if (queue->shutdown) {
        pthread_mutex_unlock(&queue->lock);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Add task */
    memcpy(&queue->tasks[queue->tail], task, sizeof(buckets_migration_task_t));
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;
    
    /* Signal consumers */
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->lock);
    
    return BUCKETS_OK;
}

/**
 * Pop task from queue (blocking if empty)
 * Returns true if task retrieved, false if shutdown
 */
static bool task_queue_pop(task_queue_t *queue, buckets_migration_task_t *task)
{
    pthread_mutex_lock(&queue->lock);
    
    /* Wait for tasks */
    while (queue->count == 0 && !queue->shutdown) {
        pthread_cond_wait(&queue->not_empty, &queue->lock);
    }
    
    if (queue->count == 0 && queue->shutdown) {
        pthread_mutex_unlock(&queue->lock);
        return false;  /* No more tasks */
    }
    
    /* Get task */
    memcpy(task, &queue->tasks[queue->head], sizeof(buckets_migration_task_t));
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    
    /* Signal producers */
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);
    
    return true;
}

/**
 * Shutdown queue (wake all waiting threads)
 */
static void task_queue_shutdown(task_queue_t *queue)
{
    pthread_mutex_lock(&queue->lock);
    queue->shutdown = true;
    pthread_cond_broadcast(&queue->not_empty);
    pthread_cond_broadcast(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);
}

/**
 * Free task queue
 */
static void task_queue_free(task_queue_t *queue)
{
    if (!queue) {
        return;
    }
    
    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    buckets_free(queue->tasks);
    buckets_free(queue);
}

/* ===================================================================
 * Worker Pool State
 * ===================================================================*/

struct buckets_worker_pool {
    int num_workers;                        /* Number of worker threads */
    pthread_t *threads;                     /* Worker thread handles */
    task_queue_t *queue;                    /* Task queue */
    
    /* Topologies for migration */
    buckets_cluster_topology_t *old_topology;
    buckets_cluster_topology_t *new_topology;
    
    /* Disk paths */
    char **disk_paths;
    int disk_count;
    
    /* Statistics */
    i64 tasks_completed;
    i64 tasks_failed;
    i64 bytes_migrated;
    i64 active_workers;
    time_t start_time;
    
    pthread_mutex_t stats_lock;             /* Stats protection */
    
    bool running;                           /* Workers running? */
};

/* ===================================================================
 * Migration Operations
 * ===================================================================*/

/**
 * Read object from source set
 * 
 * @param pool Worker pool
 * @param task Migration task
 * @param data Output buffer (caller must free)
 * @param size Output size
 * @return BUCKETS_OK on success
 */
static int read_source_object(buckets_worker_pool_t *pool,
                                buckets_migration_task_t *task,
                                u8 **data,
                                size_t *size)
{
    /* Get source set topology */
    buckets_pool_topology_t *pool_topo = &pool->old_topology->pools[task->old_pool_idx];
    buckets_set_topology_t *set = &pool_topo->sets[task->old_set_idx];
    
    if (set->disk_count == 0) {
        buckets_error("Source set has no disks");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Allocate buffer for object data */
    *data = buckets_malloc(task->size > 0 ? task->size : 1);
    if (!*data) {
        return BUCKETS_ERR_NOMEM;
    }
    
    /* TODO: Read object using storage layer API */
    /* For now, this is a placeholder that simulates reading */
    /* In production, this would call buckets_object_read() */
    *size = task->size;
    
    buckets_debug("Read object %s/%s from pool=%d set=%d (%zu bytes)",
                  task->bucket, task->object, task->old_pool_idx, task->old_set_idx, *size);
    
    return BUCKETS_OK;
}

/**
 * Write object to destination set
 * 
 * @param pool Worker pool
 * @param task Migration task
 * @param data Object data
 * @param size Object size
 * @return BUCKETS_OK on success
 */
static int write_destination_object(buckets_worker_pool_t *pool,
                                      buckets_migration_task_t *task,
                                      u8 *data,
                                      size_t size)
{
    (void)data;  /* Unused in placeholder */
    
    /* Get destination set topology */
    buckets_pool_topology_t *pool_topo = &pool->new_topology->pools[task->new_pool_idx];
    buckets_set_topology_t *set = &pool_topo->sets[task->new_set_idx];
    
    if (set->disk_count == 0) {
        buckets_error("Destination set has no disks");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* TODO: Write to all disks in set using storage layer API */
    /* For now, this is a placeholder that simulates writing */
    /* In production, this would use erasure coding and call buckets_object_write() */
    
    buckets_debug("Wrote object %s/%s to pool=%d set=%d (%zu bytes)",
                  task->bucket, task->object, task->new_pool_idx, task->new_set_idx, size);
    
    return BUCKETS_OK;
}

/**
 * Update registry with new location
 * 
 * @param pool Worker pool
 * @param task Migration task
 * @return BUCKETS_OK on success
 */
static int update_registry(buckets_worker_pool_t *pool, buckets_migration_task_t *task)
{
    (void)pool;  /* Unused in placeholder */
    
    /* TODO: Update registry to point to new location */
    /* This would use buckets_registry_record() in production */
    
    buckets_debug("Registry update: %s/%s -> pool=%d set=%d",
                  task->bucket, task->object, task->new_pool_idx, task->new_set_idx);
    
    return BUCKETS_OK;
}

/**
 * Delete object from source set
 * 
 * @param pool Worker pool
 * @param task Migration task
 * @return BUCKETS_OK on success
 */
static int delete_source_object(buckets_worker_pool_t *pool, buckets_migration_task_t *task)
{
    /* Get source set topology */
    buckets_pool_topology_t *pool_topo = &pool->old_topology->pools[task->old_pool_idx];
    buckets_set_topology_t *set = &pool_topo->sets[task->old_set_idx];
    
    /* TODO: Delete from all disks in source set using storage layer API */
    /* For now, this is a placeholder that simulates deletion */
    /* In production, this would call buckets_object_delete() on all disks */
    
    (void)set;  /* Unused in placeholder */
    
    buckets_debug("Deleted source: %s/%s from pool=%d set=%d",
                  task->bucket, task->object, task->old_pool_idx, task->old_set_idx);
    
    return BUCKETS_OK;
}

/**
 * Execute migration task
 * 
 * @param pool Worker pool
 * @param task Migration task
 * @return BUCKETS_OK on success
 */
static int execute_migration(buckets_worker_pool_t *pool, buckets_migration_task_t *task)
{
    int ret;
    u8 *data = NULL;
    size_t size = 0;
    
    buckets_info("Migrating %s/%s (%lld bytes) from pool=%d/set=%d to pool=%d/set=%d",
                 task->bucket, task->object, (long long)task->size,
                 task->old_pool_idx, task->old_set_idx,
                 task->new_pool_idx, task->new_set_idx);
    
    /* Step 1: Read from source */
    ret = read_source_object(pool, task, &data, &size);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to read source object: %s/%s", task->bucket, task->object);
        return ret;
    }
    
    /* Step 2: Write to destination */
    ret = write_destination_object(pool, task, data, size);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to write destination object: %s/%s", task->bucket, task->object);
        buckets_free(data);
        return ret;
    }
    
    /* Step 3: Update registry */
    ret = update_registry(pool, task);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to update registry: %s/%s", task->bucket, task->object);
        buckets_free(data);
        return ret;
    }
    
    /* Step 4: Delete from source */
    ret = delete_source_object(pool, task);
    if (ret != BUCKETS_OK) {
        buckets_warn("Failed to delete source object: %s/%s (non-fatal)", 
                     task->bucket, task->object);
        /* Non-fatal - object will be cleaned up later */
    }
    
    buckets_free(data);
    
    /* Update stats */
    pthread_mutex_lock(&pool->stats_lock);
    pool->tasks_completed++;
    pool->bytes_migrated += task->size;
    pthread_mutex_unlock(&pool->stats_lock);
    
    return BUCKETS_OK;
}

/**
 * Execute migration with retries
 * 
 * @param pool Worker pool
 * @param task Migration task
 * @return BUCKETS_OK on success
 */
static int execute_migration_with_retry(buckets_worker_pool_t *pool, 
                                          buckets_migration_task_t *task)
{
    int attempts = 0;
    int delay_ms = INITIAL_RETRY_DELAY_MS;
    
    while (attempts < MAX_RETRY_ATTEMPTS) {
        int ret = execute_migration(pool, task);
        
        if (ret == BUCKETS_OK) {
            return BUCKETS_OK;
        }
        
        attempts++;
        task->retry_count = attempts;
        
        if (attempts < MAX_RETRY_ATTEMPTS) {
            buckets_warn("Migration failed (attempt %d/%d), retrying in %d ms: %s/%s",
                         attempts, MAX_RETRY_ATTEMPTS, delay_ms,
                         task->bucket, task->object);
            
            /* Sleep using nanosleep */
            struct timespec ts;
            ts.tv_sec = delay_ms / 1000;
            ts.tv_nsec = (delay_ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
            
            /* Exponential backoff */
            delay_ms *= 2;
            if (delay_ms > MAX_RETRY_DELAY_MS) {
                delay_ms = MAX_RETRY_DELAY_MS;
            }
        }
    }
    
    /* All retries exhausted */
    buckets_error("Migration failed after %d attempts: %s/%s",
                  MAX_RETRY_ATTEMPTS, task->bucket, task->object);
    
    pthread_mutex_lock(&pool->stats_lock);
    pool->tasks_failed++;
    pthread_mutex_unlock(&pool->stats_lock);
    
    return BUCKETS_ERR_IO;
}

/* ===================================================================
 * Worker Thread
 * ===================================================================*/

/**
 * Worker thread main function
 */
static void* worker_thread_main(void *arg)
{
    buckets_worker_pool_t *pool = (buckets_worker_pool_t*)arg;
    buckets_migration_task_t task;
    
    buckets_debug("Worker thread started");
    
    /* Process tasks until shutdown */
    while (task_queue_pop(pool->queue, &task)) {
        /* Mark as active */
        pthread_mutex_lock(&pool->stats_lock);
        pool->active_workers++;
        pthread_mutex_unlock(&pool->stats_lock);
        
        /* Execute migration */
        execute_migration_with_retry(pool, &task);
        
        /* Mark as idle */
        pthread_mutex_lock(&pool->stats_lock);
        pool->active_workers--;
        pthread_mutex_unlock(&pool->stats_lock);
    }
    
    buckets_debug("Worker thread exiting");
    
    return NULL;
}

/* ===================================================================
 * Public API
 * ===================================================================*/

buckets_worker_pool_t* buckets_worker_pool_create(int num_workers,
                                                    buckets_cluster_topology_t *old_topology,
                                                    buckets_cluster_topology_t *new_topology,
                                                    char **disk_paths,
                                                    int disk_count)
{
    if (!old_topology || !new_topology || !disk_paths || disk_count <= 0) {
        return NULL;
    }
    
    if (num_workers <= 0) {
        num_workers = DEFAULT_NUM_WORKERS;
    }
    
    buckets_worker_pool_t *pool = buckets_calloc(1, sizeof(buckets_worker_pool_t));
    if (!pool) {
        return NULL;
    }
    
    pool->num_workers = num_workers;
    pool->old_topology = old_topology;
    pool->new_topology = new_topology;
    pool->disk_paths = disk_paths;
    pool->disk_count = disk_count;
    pool->running = false;
    
    /* Initialize stats */
    pool->tasks_completed = 0;
    pool->tasks_failed = 0;
    pool->bytes_migrated = 0;
    pool->active_workers = 0;
    pool->start_time = time(NULL);
    
    pthread_mutex_init(&pool->stats_lock, NULL);
    
    /* Create task queue (10000 task capacity) */
    pool->queue = task_queue_init(10000);
    if (!pool->queue) {
        buckets_free(pool);
        return NULL;
    }
    
    /* Allocate thread array */
    pool->threads = buckets_calloc(num_workers, sizeof(pthread_t));
    if (!pool->threads) {
        task_queue_free(pool->queue);
        buckets_free(pool);
        return NULL;
    }
    
    buckets_info("Created worker pool with %d workers", num_workers);
    
    return pool;
}

int buckets_worker_pool_start(buckets_worker_pool_t *pool)
{
    if (!pool || pool->running) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Start worker threads */
    for (int i = 0; i < pool->num_workers; i++) {
        int ret = pthread_create(&pool->threads[i], NULL, worker_thread_main, pool);
        if (ret != 0) {
            buckets_error("Failed to create worker thread %d: %s", i, strerror(ret));
            /* Stop already-started threads */
            buckets_worker_pool_stop(pool);
            return BUCKETS_ERR_IO;
        }
    }
    
    pool->running = true;
    pool->start_time = time(NULL);
    
    buckets_info("Started %d worker threads", pool->num_workers);
    
    return BUCKETS_OK;
}

int buckets_worker_pool_submit(buckets_worker_pool_t *pool,
                                 buckets_migration_task_t *tasks,
                                 int task_count)
{
    if (!pool || !tasks || task_count <= 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (!pool->running) {
        buckets_error("Worker pool not running");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Submit all tasks to queue */
    for (int i = 0; i < task_count; i++) {
        int ret = task_queue_push(pool->queue, &tasks[i]);
        if (ret != BUCKETS_OK) {
            buckets_error("Failed to submit task %d", i);
            return ret;
        }
    }
    
    buckets_info("Submitted %d tasks to worker pool", task_count);
    
    return BUCKETS_OK;
}

int buckets_worker_pool_wait(buckets_worker_pool_t *pool)
{
    if (!pool) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Wait for queue to drain */
    buckets_info("Waiting for all tasks to complete...");
    
    while (true) {
        pthread_mutex_lock(&pool->queue->lock);
        int remaining = pool->queue->count;
        pthread_mutex_unlock(&pool->queue->lock);
        
        pthread_mutex_lock(&pool->stats_lock);
        i64 active = pool->active_workers;
        pthread_mutex_unlock(&pool->stats_lock);
        
        if (remaining == 0 && active == 0) {
            break;  /* All done */
        }
        
        /* Sleep 100ms */
        struct timespec ts = {0, 100000000L};  /* 100ms = 100,000,000 ns */
        nanosleep(&ts, NULL);
    }
    
    buckets_info("All tasks completed");
    
    return BUCKETS_OK;
}

int buckets_worker_pool_stop(buckets_worker_pool_t *pool)
{
    if (!pool || !pool->running) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    buckets_info("Stopping worker pool...");
    
    /* Signal shutdown */
    task_queue_shutdown(pool->queue);
    
    /* Join all threads */
    for (int i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    pool->running = false;
    
    buckets_info("Worker pool stopped");
    
    return BUCKETS_OK;
}

int buckets_worker_pool_get_stats(buckets_worker_pool_t *pool,
                                    buckets_worker_stats_t *stats)
{
    if (!pool || !stats) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&pool->stats_lock);
    
    stats->tasks_completed = pool->tasks_completed;
    stats->tasks_failed = pool->tasks_failed;
    stats->bytes_migrated = pool->bytes_migrated;
    stats->active_workers = pool->active_workers;
    stats->idle_workers = pool->num_workers - pool->active_workers;
    
    /* Calculate throughput */
    time_t elapsed = time(NULL) - pool->start_time;
    if (elapsed > 0) {
        double mb = (double)pool->bytes_migrated / (1024.0 * 1024.0);
        stats->throughput_mbps = mb / (double)elapsed;
    } else {
        stats->throughput_mbps = 0.0;
    }
    
    pthread_mutex_unlock(&pool->stats_lock);
    
    return BUCKETS_OK;
}

void buckets_worker_pool_free(buckets_worker_pool_t *pool)
{
    if (!pool) {
        return;
    }
    
    if (pool->running) {
        buckets_worker_pool_stop(pool);
    }
    
    task_queue_free(pool->queue);
    pthread_mutex_destroy(&pool->stats_lock);
    buckets_free(pool->threads);
    buckets_free(pool);
    
    buckets_info("Worker pool freed");
}
