/**
 * Topology Manager Implementation
 * 
 * High-level coordination layer for topology changes across the cluster.
 * Handles:
 * - Topology change coordination with generation tracking
 * - Automatic quorum persistence (write to all disks)
 * - Topology reload from disk with quorum consensus
 * - Event callbacks for topology changes
 * - Thread-safe access to current topology
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_cache.h"

/* ===================================================================
 * Topology Manager Singleton
 * =================================================================== */

typedef void (*buckets_topology_change_callback_t)(buckets_cluster_topology_t *topology,
                                                    void *user_data);

typedef struct {
    char **disk_paths;                          /* Array of disk paths */
    int disk_count;                             /* Number of disks */
    buckets_topology_change_callback_t callback; /* Change notification callback */
    void *callback_user_data;                   /* User data for callback */
    pthread_mutex_t lock;                       /* Mutex for change operations */
    bool initialized;                           /* Is manager initialized? */
} buckets_topology_manager_t;

static buckets_topology_manager_t g_topology_manager = {
    .disk_paths = NULL,
    .disk_count = 0,
    .callback = NULL,
    .callback_user_data = NULL,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .initialized = false
};

/* ===================================================================
 * Initialization & Cleanup
 * =================================================================== */

int buckets_topology_manager_init(char **disk_paths, int disk_count)
{
    if (!disk_paths || disk_count <= 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&g_topology_manager.lock);
    
    if (g_topology_manager.initialized) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        buckets_warn("Topology manager already initialized");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Copy disk paths */
    g_topology_manager.disk_count = disk_count;
    g_topology_manager.disk_paths = buckets_calloc(disk_count, sizeof(char*));
    
    for (int i = 0; i < disk_count; i++) {
        g_topology_manager.disk_paths[i] = buckets_strdup(disk_paths[i]);
    }
    
    g_topology_manager.callback = NULL;
    g_topology_manager.callback_user_data = NULL;
    g_topology_manager.initialized = true;
    
    pthread_mutex_unlock(&g_topology_manager.lock);
    
    buckets_info("Topology manager initialized with %d disks", disk_count);
    
    return BUCKETS_OK;
}

void buckets_topology_manager_cleanup(void)
{
    pthread_mutex_lock(&g_topology_manager.lock);
    
    if (!g_topology_manager.initialized) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        return;
    }
    
    /* Free disk paths */
    for (int i = 0; i < g_topology_manager.disk_count; i++) {
        buckets_free(g_topology_manager.disk_paths[i]);
    }
    buckets_free(g_topology_manager.disk_paths);
    
    g_topology_manager.disk_paths = NULL;
    g_topology_manager.disk_count = 0;
    g_topology_manager.callback = NULL;
    g_topology_manager.callback_user_data = NULL;
    g_topology_manager.initialized = false;
    
    pthread_mutex_unlock(&g_topology_manager.lock);
    
    buckets_info("Topology manager cleaned up");
}

/* ===================================================================
 * Topology Access (Read-Only)
 * =================================================================== */

buckets_cluster_topology_t* buckets_topology_manager_get(void)
{
    if (!g_topology_manager.initialized) {
        buckets_warn("Topology manager not initialized");
        return NULL;
    }
    
    /* Delegate to topology cache (already thread-safe) */
    return buckets_topology_cache_get();
}

/* ===================================================================
 * Topology Loading (Quorum-Based)
 * =================================================================== */

int buckets_topology_manager_load(void)
{
    if (!g_topology_manager.initialized) {
        buckets_warn("Topology manager not initialized");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&g_topology_manager.lock);
    
    buckets_info("Loading topology from %d disks with quorum",
                 g_topology_manager.disk_count);
    
    /* Load with quorum consensus */
    buckets_cluster_topology_t *topology = 
        buckets_topology_load_quorum(g_topology_manager.disk_paths,
                                      g_topology_manager.disk_count);
    
    if (!topology) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        buckets_error("Failed to load topology with quorum");
        return BUCKETS_ERR_QUORUM;
    }
    
    /* Update cache (cache takes ownership) */
    int ret = buckets_topology_cache_set(topology);
    if (ret != BUCKETS_OK) {
        buckets_topology_free(topology);
        pthread_mutex_unlock(&g_topology_manager.lock);
        return ret;
    }
    
    buckets_info("Topology loaded successfully: generation=%ld, pools=%d",
                 topology->generation, topology->pool_count);
    
    pthread_mutex_unlock(&g_topology_manager.lock);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Topology Changes (Coordinated)
 * =================================================================== */

/**
 * Clone a topology (deep copy)
 * Caller must free the clone with buckets_topology_free()
 */
static buckets_cluster_topology_t* clone_topology(buckets_cluster_topology_t *topology)
{
    if (!topology) {
        return NULL;
    }
    
    buckets_cluster_topology_t *clone = buckets_calloc(1, sizeof(buckets_cluster_topology_t));
    memcpy(clone, topology, sizeof(buckets_cluster_topology_t));
    
    /* Deep copy pools */
    if (topology->pool_count > 0) {
        clone->pools = buckets_calloc(topology->pool_count, sizeof(buckets_pool_topology_t));
        for (int i = 0; i < topology->pool_count; i++) {
            buckets_pool_topology_t *src_pool = &topology->pools[i];
            buckets_pool_topology_t *dst_pool = &clone->pools[i];
            
            dst_pool->idx = src_pool->idx;
            dst_pool->set_count = src_pool->set_count;
            
            if (src_pool->set_count > 0) {
                dst_pool->sets = buckets_calloc(src_pool->set_count, sizeof(buckets_set_topology_t));
                for (int j = 0; j < src_pool->set_count; j++) {
                    buckets_set_topology_t *src_set = &src_pool->sets[j];
                    buckets_set_topology_t *dst_set = &dst_pool->sets[j];
                    
                    dst_set->idx = src_set->idx;
                    dst_set->state = src_set->state;
                    dst_set->disk_count = src_set->disk_count;
                    
                    if (src_set->disk_count > 0) {
                        dst_set->disks = buckets_calloc(src_set->disk_count, sizeof(buckets_disk_info_t));
                        memcpy(dst_set->disks, src_set->disks,
                               src_set->disk_count * sizeof(buckets_disk_info_t));
                    }
                }
            }
        }
    }
    
    return clone;
}

static int persist_and_notify(buckets_cluster_topology_t *topology)
{
    /* Save with quorum */
    int ret = buckets_topology_save_quorum(g_topology_manager.disk_paths,
                                            g_topology_manager.disk_count,
                                            topology);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to persist topology with quorum");
        return ret;
    }
    
    /* Update cache with cloned copy (cache takes ownership) */
    buckets_cluster_topology_t *clone = clone_topology(topology);
    if (!clone) {
        return BUCKETS_ERR_NOMEM;
    }
    
    ret = buckets_topology_cache_set(clone);
    if (ret != BUCKETS_OK) {
        buckets_topology_free(clone);
        return ret;
    }
    
    /* Invoke callback if registered */
    if (g_topology_manager.callback) {
        g_topology_manager.callback(topology, g_topology_manager.callback_user_data);
    }
    
    buckets_info("Topology change persisted and cached: generation=%ld",
                 topology->generation);
    
    return BUCKETS_OK;
}

int buckets_topology_manager_add_pool(void)
{
    if (!g_topology_manager.initialized) {
        buckets_warn("Topology manager not initialized");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&g_topology_manager.lock);
    
    /* Get current topology and clone it */
    buckets_cluster_topology_t *cached = buckets_topology_cache_get();
    if (!cached) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        buckets_error("No topology available to modify");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    buckets_cluster_topology_t *topology = clone_topology(cached);
    if (!topology) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Add pool (modifies clone) */
    int ret = buckets_topology_add_pool(topology);
    if (ret != BUCKETS_OK) {
        buckets_topology_free(topology);
        pthread_mutex_unlock(&g_topology_manager.lock);
        return ret;
    }
    
    /* Persist and notify */
    ret = persist_and_notify(topology);
    
    /* Free the working copy */
    buckets_topology_free(topology);
    
    pthread_mutex_unlock(&g_topology_manager.lock);
    
    return ret;
}

int buckets_topology_manager_add_set(int pool_idx, buckets_disk_info_t *disks, int disk_count)
{
    if (!g_topology_manager.initialized) {
        buckets_warn("Topology manager not initialized");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (!disks || disk_count <= 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&g_topology_manager.lock);
    
    /* Get current topology and clone it */
    buckets_cluster_topology_t *cached = buckets_topology_cache_get();
    if (!cached) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        buckets_error("No topology available to modify");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    buckets_cluster_topology_t *topology = clone_topology(cached);
    if (!topology) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Add set (modifies clone) */
    int ret = buckets_topology_add_set(topology, pool_idx, disks, disk_count);
    if (ret != BUCKETS_OK) {
        buckets_topology_free(topology);
        pthread_mutex_unlock(&g_topology_manager.lock);
        return ret;
    }
    
    /* Persist and notify */
    ret = persist_and_notify(topology);
    
    /* Free the working copy */
    buckets_topology_free(topology);
    
    pthread_mutex_unlock(&g_topology_manager.lock);
    
    return ret;
}

int buckets_topology_manager_mark_set_draining(int pool_idx, int set_idx)
{
    if (!g_topology_manager.initialized) {
        buckets_warn("Topology manager not initialized");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&g_topology_manager.lock);
    
    /* Get current topology and clone it */
    buckets_cluster_topology_t *cached = buckets_topology_cache_get();
    if (!cached) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        buckets_error("No topology available to modify");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    buckets_cluster_topology_t *topology = clone_topology(cached);
    if (!topology) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Mark set draining (modifies clone) */
    int ret = buckets_topology_mark_set_draining(topology, pool_idx, set_idx);
    if (ret != BUCKETS_OK) {
        buckets_topology_free(topology);
        pthread_mutex_unlock(&g_topology_manager.lock);
        return ret;
    }
    
    /* Persist and notify */
    ret = persist_and_notify(topology);
    
    /* Free the working copy */
    buckets_topology_free(topology);
    
    pthread_mutex_unlock(&g_topology_manager.lock);
    
    return ret;
}

int buckets_topology_manager_mark_set_removed(int pool_idx, int set_idx)
{
    if (!g_topology_manager.initialized) {
        buckets_warn("Topology manager not initialized");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&g_topology_manager.lock);
    
    /* Get current topology and clone it */
    buckets_cluster_topology_t *cached = buckets_topology_cache_get();
    if (!cached) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        buckets_error("No topology available to modify");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    buckets_cluster_topology_t *topology = clone_topology(cached);
    if (!topology) {
        pthread_mutex_unlock(&g_topology_manager.lock);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Mark set removed (modifies clone) */
    int ret = buckets_topology_mark_set_removed(topology, pool_idx, set_idx);
    if (ret != BUCKETS_OK) {
        buckets_topology_free(topology);
        pthread_mutex_unlock(&g_topology_manager.lock);
        return ret;
    }
    
    /* Persist and notify */
    ret = persist_and_notify(topology);
    
    /* Free the working copy */
    buckets_topology_free(topology);
    
    pthread_mutex_unlock(&g_topology_manager.lock);
    
    return ret;
}

/* ===================================================================
 * Event Callbacks
 * =================================================================== */

int buckets_topology_manager_set_callback(buckets_topology_change_callback_t callback,
                                          void *user_data)
{
    if (!g_topology_manager.initialized) {
        buckets_warn("Topology manager not initialized");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&g_topology_manager.lock);
    
    g_topology_manager.callback = callback;
    g_topology_manager.callback_user_data = user_data;
    
    pthread_mutex_unlock(&g_topology_manager.lock);
    
    buckets_info("Topology change callback registered");
    
    return BUCKETS_OK;
}
