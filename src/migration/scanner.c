/**
 * Migration Scanner Implementation
 * 
 * Enumerates objects across all disks and identifies which objects need
 * migration based on topology changes.
 * 
 * Approach:
 * 1. Walk all disks in parallel (one thread per disk)
 * 2. For each xl.meta file found, extract object metadata
 * 3. Compute old and new locations using hash ring
 * 4. If locations differ, add to migration queue
 * 5. Sort queue by size (small objects first for quick wins)
 */

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_migration.h"
#include "buckets_ring.h"
#include "buckets_storage.h"

/* ===================================================================
 * Per-Disk Scanner Thread
 * ===================================================================*/

/* Node ID encoding: node_id = pool_idx * 1000 + set_idx */
#define ENCODE_NODE_ID(pool, set) ((pool) * 1000 + (set))
#define DECODE_POOL(node_id) ((node_id) / 1000)
#define DECODE_SET(node_id) ((node_id) % 1000)

/**
 * Context for per-disk scanner thread
 */
typedef struct {
    char *disk_path;                        /* Disk to scan */
    buckets_cluster_topology_t *old_topo;   /* Old topology */
    buckets_cluster_topology_t *new_topo;   /* New topology */
    buckets_ring_t *old_ring;               /* Old hash ring */
    buckets_ring_t *new_ring;               /* New hash ring */
    
    /* Results (accessed with scanner lock) */
    buckets_migration_task_t *tasks;        /* Task array */
    int task_count;                         /* Current count */
    int task_capacity;                      /* Array capacity */
    
    i64 objects_scanned;                    /* Stats */
    i64 objects_affected;
    i64 bytes_affected;
    
    buckets_scanner_state_t *scanner;       /* Parent scanner */
} disk_scanner_ctx_t;

/**
 * Check if object needs migration
 * 
 * Returns true if object's computed location changed between topologies
 */
static bool needs_migration(disk_scanner_ctx_t *ctx, const char *bucket, const char *object,
                            int *old_pool, int *old_set, int *new_pool, int *new_set)
{
    /* Compute old location */
    char object_key[2048];
    snprintf(object_key, sizeof(object_key), "%s/%s", bucket, object);
    
    i32 old_node_id = buckets_ring_lookup(ctx->old_ring, object_key);
    if (old_node_id < 0) {
        buckets_warn("Failed to compute old location for %s", object_key);
        return false;
    }
    
    /* Compute new location */
    i32 new_node_id = buckets_ring_lookup(ctx->new_ring, object_key);
    if (new_node_id < 0) {
        buckets_warn("Failed to compute new location for %s", object_key);
        return false;
    }
    
    /* Decode pool/set from node IDs */
    *old_pool = DECODE_POOL(old_node_id);
    *old_set = DECODE_SET(old_node_id);
    *new_pool = DECODE_POOL(new_node_id);
    *new_set = DECODE_SET(new_node_id);
    
    /* Different location = needs migration */
    return (old_node_id != new_node_id);
}

/**
 * Add task to migration queue
 */
static int add_migration_task(disk_scanner_ctx_t *ctx, const char *bucket,
                               const char *object, const char *version_id,
                               i64 size, time_t mod_time,
                               int old_pool, int old_set,
                               int new_pool, int new_set)
{
    /* Grow array if needed */
    if (ctx->task_count >= ctx->task_capacity) {
        int new_capacity = ctx->task_capacity * 2;
        buckets_migration_task_t *new_tasks = buckets_realloc(
            ctx->tasks, new_capacity * sizeof(buckets_migration_task_t));
        if (!new_tasks) {
            return BUCKETS_ERR_NOMEM;
        }
        ctx->tasks = new_tasks;
        ctx->task_capacity = new_capacity;
    }
    
    /* Add task */
    buckets_migration_task_t *task = &ctx->tasks[ctx->task_count++];
    snprintf(task->bucket, sizeof(task->bucket), "%s", bucket);
    snprintf(task->object, sizeof(task->object), "%s", object);
    snprintf(task->version_id, sizeof(task->version_id), "%s", version_id ? version_id : "");
    
    task->old_pool_idx = old_pool;
    task->old_set_idx = old_set;
    task->new_pool_idx = new_pool;
    task->new_set_idx = new_set;
    
    task->size = size;
    task->mod_time = mod_time;
    task->retry_count = 0;
    task->last_attempt = 0;
    
    ctx->objects_affected++;
    ctx->bytes_affected += size;
    
    return BUCKETS_OK;
}

/**
 * Scan directory recursively for xl.meta files
 */
static int scan_directory(disk_scanner_ctx_t *ctx, const char *dir_path,
                          const char *bucket, const char *prefix)
{
    DIR *dir = opendir(dir_path);
    if (!dir) {
        if (errno != ENOENT) {
            buckets_warn("Failed to open directory %s: %s", dir_path, strerror(errno));
        }
        return BUCKETS_OK;  /* Not a fatal error */
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Skip .minio.sys and .buckets.sys */
        if (strcmp(entry->d_name, ".minio.sys") == 0 ||
            strcmp(entry->d_name, ".buckets.sys") == 0) {
            continue;
        }
        
        /* Build full path */
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        
        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }
        
        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            char new_prefix[2048];
            if (prefix[0] == '\0') {
                snprintf(new_prefix, sizeof(new_prefix), "%s", entry->d_name);
            } else {
                snprintf(new_prefix, sizeof(new_prefix), "%s/%s", prefix, entry->d_name);
            }
            scan_directory(ctx, full_path, bucket, new_prefix);
        } else if (strcmp(entry->d_name, "xl.meta") == 0) {
            /* Found xl.meta - this is an object */
            ctx->objects_scanned++;
            
            /* Object key is the prefix */
            const char *object = prefix;
            
            /* Check if migration needed */
            int old_pool, old_set, new_pool, new_set;
            if (!needs_migration(ctx, bucket, object, &old_pool, &old_set, &new_pool, &new_set)) {
                continue;  /* No migration needed */
            }
            
            /* Add to migration queue */
            add_migration_task(ctx, bucket, object, NULL, st.st_size, st.st_mtime,
                               old_pool, old_set, new_pool, new_set);
        }
    }
    
    closedir(dir);
    return BUCKETS_OK;
}

/**
 * Scan buckets on a disk
 */
static int scan_disk_buckets(disk_scanner_ctx_t *ctx)
{
    DIR *dir = opendir(ctx->disk_path);
    if (!dir) {
        buckets_error("Failed to open disk %s: %s", ctx->disk_path, strerror(errno));
        return BUCKETS_ERR_IO;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip special entries */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ".minio.sys") == 0 ||
            strcmp(entry->d_name, ".buckets.sys") == 0) {
            continue;
        }
        
        char bucket_path[4096];
        snprintf(bucket_path, sizeof(bucket_path), "%s/%s", ctx->disk_path, entry->d_name);
        
        struct stat st;
        if (stat(bucket_path, &st) != 0) {
            continue;
        }
        
        if (S_ISDIR(st.st_mode)) {
            /* This is a bucket - scan it */
            const char *bucket = entry->d_name;
            scan_directory(ctx, bucket_path, bucket, "");
        }
    }
    
    closedir(dir);
    return BUCKETS_OK;
}

/**
 * Per-disk scanner thread
 */
static void* disk_scanner_thread(void *arg)
{
    disk_scanner_ctx_t *ctx = (disk_scanner_ctx_t*)arg;
    
    buckets_debug("Scanning disk: %s", ctx->disk_path);
    
    /* Scan all buckets on this disk */
    scan_disk_buckets(ctx);
    
    /* Update scanner stats (thread-safe) */
    pthread_mutex_lock(&ctx->scanner->lock);
    ctx->scanner->objects_scanned += ctx->objects_scanned;
    ctx->scanner->objects_affected += ctx->objects_affected;
    ctx->scanner->bytes_affected += ctx->bytes_affected;
    pthread_mutex_unlock(&ctx->scanner->lock);
    
    buckets_debug("Disk %s: scanned %ld objects, %ld need migration",
                  ctx->disk_path, ctx->objects_scanned, ctx->objects_affected);
    
    return NULL;
}

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/**
 * Build hash ring from topology
 * 
 * Creates a consistent hash ring with all sets from the topology.
 * Each set becomes a node in the ring.
 * 
 * Node IDs are encoded as: pool_idx * 1000 + set_idx
 */
static buckets_ring_t* topology_to_ring(buckets_cluster_topology_t *topology)
{
    if (!topology) {
        return NULL;
    }
    
    /* Create ring with default vnode factor (150) and seed 0 */
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    if (!ring) {
        return NULL;
    }
    
    /* Add each set as a node in the ring */
    for (int p = 0; p < topology->pool_count; p++) {
        buckets_pool_topology_t *pool = &topology->pools[p];
        for (int s = 0; s < pool->set_count; s++) {
            i32 node_id = ENCODE_NODE_ID(p, s);
            char node_name[64];
            snprintf(node_name, sizeof(node_name), "pool%d-set%d", p, s);
            
            buckets_error_t ret = buckets_ring_add_node(ring, node_id, node_name);
            if (ret != BUCKETS_OK) {
                buckets_warn("Failed to add node pool%d-set%d to ring", p, s);
                buckets_ring_free(ring);
                return NULL;
            }
        }
    }
    
    return ring;
}

/**
 * Compare function for sorting tasks by size (ascending)
 */
static int compare_tasks_by_size(const void *a, const void *b)
{
    const buckets_migration_task_t *task_a = (const buckets_migration_task_t*)a;
    const buckets_migration_task_t *task_b = (const buckets_migration_task_t*)b;
    
    if (task_a->size < task_b->size) return -1;
    if (task_a->size > task_b->size) return 1;
    return 0;
}

/* ===================================================================
 * Public API
 * ===================================================================*/

buckets_scanner_state_t* buckets_scanner_init(char **disk_paths, int disk_count,
                                               buckets_cluster_topology_t *old_topology,
                                               buckets_cluster_topology_t *new_topology)
{
    if (!disk_paths || disk_count <= 0 || !old_topology || !new_topology) {
        return NULL;
    }
    
    buckets_scanner_state_t *scanner = buckets_calloc(1, sizeof(buckets_scanner_state_t));
    if (!scanner) {
        return NULL;
    }
    
    /* Copy disk paths */
    scanner->disk_paths = buckets_calloc(disk_count, sizeof(char*));
    if (!scanner->disk_paths) {
        buckets_free(scanner);
        return NULL;
    }
    
    for (int i = 0; i < disk_count; i++) {
        scanner->disk_paths[i] = buckets_strdup(disk_paths[i]);
    }
    scanner->disk_count = disk_count;
    
    /* Store topologies (we don't own these, just reference them) */
    scanner->old_topology = old_topology;
    scanner->new_topology = new_topology;
    
    /* Initialize stats */
    scanner->objects_scanned = 0;
    scanner->objects_affected = 0;
    scanner->bytes_affected = 0;
    scanner->last_bucket = NULL;
    scanner->last_object = NULL;
    scanner->scan_complete = false;
    
    pthread_mutex_init(&scanner->lock, NULL);
    
    buckets_info("Scanner initialized with %d disks", disk_count);
    
    return scanner;
}

int buckets_scanner_scan(buckets_scanner_state_t *scanner,
                         buckets_migration_task_t **queue,
                         int *queue_size,
                         int *task_count)
{
    if (!scanner || !queue || !queue_size || !task_count) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    buckets_info("Starting parallel disk scan (%d disks)...", scanner->disk_count);
    
    /* Build hash rings from topologies */
    buckets_ring_t *old_ring = topology_to_ring(scanner->old_topology);
    buckets_ring_t *new_ring = topology_to_ring(scanner->new_topology);
    
    if (!old_ring || !new_ring) {
        buckets_error("Failed to build hash rings from topologies");
        if (old_ring) buckets_ring_free(old_ring);
        if (new_ring) buckets_ring_free(new_ring);
        return BUCKETS_ERR_NOMEM;
    }
    
    buckets_debug("Built hash rings: old=%zu nodes, new=%zu nodes",
                  old_ring->node_count,
                  new_ring->node_count);
    
    /* Create per-disk scanner contexts */
    disk_scanner_ctx_t *contexts = buckets_calloc(scanner->disk_count, sizeof(disk_scanner_ctx_t));
    pthread_t *threads = buckets_calloc(scanner->disk_count, sizeof(pthread_t));
    
    if (!contexts || !threads) {
        buckets_free(contexts);
        buckets_free(threads);
        buckets_ring_free(old_ring);
        buckets_ring_free(new_ring);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Initialize contexts and start threads */
    for (int i = 0; i < scanner->disk_count; i++) {
        contexts[i].disk_path = scanner->disk_paths[i];
        contexts[i].old_topo = scanner->old_topology;
        contexts[i].new_topo = scanner->new_topology;
        contexts[i].old_ring = old_ring;
        contexts[i].new_ring = new_ring;
        contexts[i].scanner = scanner;
        
        /* Allocate initial task array */
        contexts[i].task_capacity = 1000;
        contexts[i].task_count = 0;
        contexts[i].tasks = buckets_calloc(contexts[i].task_capacity,
                                           sizeof(buckets_migration_task_t));
        
        contexts[i].objects_scanned = 0;
        contexts[i].objects_affected = 0;
        contexts[i].bytes_affected = 0;
        
        pthread_create(&threads[i], NULL, disk_scanner_thread, &contexts[i]);
    }
    
    /* Wait for all threads to complete */
    for (int i = 0; i < scanner->disk_count; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* Merge all task arrays */
    int total_tasks = 0;
    for (int i = 0; i < scanner->disk_count; i++) {
        total_tasks += contexts[i].task_count;
    }
    
    if (total_tasks == 0) {
        buckets_info("No objects need migration");
        *queue = NULL;
        *queue_size = 0;
        *task_count = 0;
        scanner->scan_complete = true;
        
        /* Cleanup */
        for (int i = 0; i < scanner->disk_count; i++) {
            buckets_free(contexts[i].tasks);
        }
        buckets_free(contexts);
        buckets_free(threads);
        buckets_ring_free(old_ring);
        buckets_ring_free(new_ring);
        
        return BUCKETS_OK;
    }
    
    /* Allocate merged array */
    buckets_migration_task_t *merged = buckets_calloc(total_tasks,
                                                      sizeof(buckets_migration_task_t));
    if (!merged) {
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Copy all tasks */
    int offset = 0;
    for (int i = 0; i < scanner->disk_count; i++) {
        memcpy(&merged[offset], contexts[i].tasks,
               contexts[i].task_count * sizeof(buckets_migration_task_t));
        offset += contexts[i].task_count;
    }
    
    /* Sort by size (small objects first for quick wins) */
    qsort(merged, total_tasks, sizeof(buckets_migration_task_t), compare_tasks_by_size);
    buckets_debug("Sorted %d tasks by size (smallest first)", total_tasks);
    
    *queue = merged;
    *queue_size = total_tasks;
    *task_count = total_tasks;
    scanner->scan_complete = true;
    
    buckets_info("Scan complete: %ld objects scanned, %d need migration (%ld MB)",
                 scanner->objects_scanned, total_tasks,
                 scanner->bytes_affected / (1024 * 1024));
    
    /* Cleanup contexts */
    for (int i = 0; i < scanner->disk_count; i++) {
        buckets_free(contexts[i].tasks);
    }
    buckets_free(contexts);
    buckets_free(threads);
    
    /* Cleanup rings */
    buckets_ring_free(old_ring);
    buckets_ring_free(new_ring);
    
    return BUCKETS_OK;
}

int buckets_scanner_get_stats(buckets_scanner_state_t *scanner,
                               buckets_scanner_stats_t *stats)
{
    if (!scanner || !stats) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&scanner->lock);
    
    stats->objects_scanned = scanner->objects_scanned;
    stats->objects_affected = scanner->objects_affected;
    stats->bytes_affected = scanner->bytes_affected;
    stats->complete = scanner->scan_complete;
    
    /* Calculate progress percentage */
    if (scanner->objects_scanned > 0) {
        stats->progress_percent = (scanner->objects_affected * 100.0) / scanner->objects_scanned;
    } else {
        stats->progress_percent = 0.0;
    }
    
    pthread_mutex_unlock(&scanner->lock);
    
    return BUCKETS_OK;
}

void buckets_scanner_cleanup(buckets_scanner_state_t *scanner)
{
    if (!scanner) {
        return;
    }
    
    /* Free disk paths */
    for (int i = 0; i < scanner->disk_count; i++) {
        buckets_free(scanner->disk_paths[i]);
    }
    buckets_free(scanner->disk_paths);
    
    /* Free checkpoint data */
    buckets_free(scanner->last_bucket);
    buckets_free(scanner->last_object);
    
    pthread_mutex_destroy(&scanner->lock);
    
    buckets_free(scanner);
    
    buckets_info("Scanner cleaned up");
}
