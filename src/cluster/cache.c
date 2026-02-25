/**
 * Cache Management Implementation
 * 
 * Thread-safe global caches for format and topology metadata using rwlocks
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_cache.h"

/* ===== Format Cache ===== */

typedef struct {
    buckets_format_t *format;       /* Cached format (owned by cache) */
    pthread_rwlock_t lock;          /* Read-write lock */
    bool initialized;               /* Is cache initialized? */
} buckets_format_cache_t;

static buckets_format_cache_t g_format_cache = {
    .format = NULL,
    .lock = PTHREAD_RWLOCK_INITIALIZER,
    .initialized = false
};

void buckets_format_cache_init(void)
{
    if (g_format_cache.initialized) {
        buckets_warn("Format cache already initialized");
        return;
    }
    
    pthread_rwlock_init(&g_format_cache.lock, NULL);
    g_format_cache.format = NULL;
    g_format_cache.initialized = true;
    
    buckets_debug("Format cache initialized");
}

void buckets_format_cache_cleanup(void)
{
    if (!g_format_cache.initialized) {
        return;
    }
    
    pthread_rwlock_wrlock(&g_format_cache.lock);
    
    if (g_format_cache.format) {
        buckets_format_free(g_format_cache.format);
        g_format_cache.format = NULL;
    }
    
    pthread_rwlock_unlock(&g_format_cache.lock);
    pthread_rwlock_destroy(&g_format_cache.lock);
    
    g_format_cache.initialized = false;
    
    buckets_debug("Format cache cleaned up");
}

buckets_format_t* buckets_format_cache_get(void)
{
    if (!g_format_cache.initialized) {
        buckets_warn("Format cache not initialized");
        return NULL;
    }
    
    pthread_rwlock_rdlock(&g_format_cache.lock);
    buckets_format_t *format = g_format_cache.format;
    pthread_rwlock_unlock(&g_format_cache.lock);
    
    if (format) {
        buckets_debug("Format cache hit: deployment_id=%s", format->meta.deployment_id);
    } else {
        buckets_debug("Format cache miss");
    }
    
    return format;
}

int buckets_format_cache_set(buckets_format_t *format)
{
    if (!g_format_cache.initialized) {
        buckets_warn("Format cache not initialized");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (!format) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Clone the format */
    buckets_format_t *clone = buckets_format_clone(format);
    if (!clone) {
        return BUCKETS_ERR_NOMEM;
    }
    
    pthread_rwlock_wrlock(&g_format_cache.lock);
    
    /* Free old format if exists */
    if (g_format_cache.format) {
        buckets_format_free(g_format_cache.format);
    }
    
    g_format_cache.format = clone;
    
    pthread_rwlock_unlock(&g_format_cache.lock);
    
    buckets_info("Format cached: deployment_id=%s", format->meta.deployment_id);
    
    return BUCKETS_OK;
}

void buckets_format_cache_invalidate(void)
{
    if (!g_format_cache.initialized) {
        return;
    }
    
    pthread_rwlock_wrlock(&g_format_cache.lock);
    
    if (g_format_cache.format) {
        buckets_format_free(g_format_cache.format);
        g_format_cache.format = NULL;
        buckets_info("Format cache invalidated");
    }
    
    pthread_rwlock_unlock(&g_format_cache.lock);
}

/* ===== Topology Cache ===== */

typedef struct {
    buckets_cluster_topology_t *topology;   /* Cached topology (owned by cache) */
    pthread_rwlock_t lock;                  /* Read-write lock */
    bool initialized;                       /* Is cache initialized? */
} buckets_topology_cache_t;

static buckets_topology_cache_t g_topology_cache = {
    .topology = NULL,
    .lock = PTHREAD_RWLOCK_INITIALIZER,
    .initialized = false
};

void buckets_topology_cache_init(void)
{
    if (g_topology_cache.initialized) {
        buckets_warn("Topology cache already initialized");
        return;
    }
    
    pthread_rwlock_init(&g_topology_cache.lock, NULL);
    g_topology_cache.topology = NULL;
    g_topology_cache.initialized = true;
    
    buckets_debug("Topology cache initialized");
}

void buckets_topology_cache_cleanup(void)
{
    if (!g_topology_cache.initialized) {
        return;
    }
    
    pthread_rwlock_wrlock(&g_topology_cache.lock);
    
    if (g_topology_cache.topology) {
        buckets_topology_free(g_topology_cache.topology);
        g_topology_cache.topology = NULL;
    }
    
    pthread_rwlock_unlock(&g_topology_cache.lock);
    pthread_rwlock_destroy(&g_topology_cache.lock);
    
    g_topology_cache.initialized = false;
    
    buckets_debug("Topology cache cleaned up");
}

buckets_cluster_topology_t* buckets_topology_cache_get(void)
{
    if (!g_topology_cache.initialized) {
        buckets_warn("Topology cache not initialized");
        return NULL;
    }
    
    pthread_rwlock_rdlock(&g_topology_cache.lock);
    buckets_cluster_topology_t *topology = g_topology_cache.topology;
    pthread_rwlock_unlock(&g_topology_cache.lock);
    
    if (topology) {
        buckets_debug("Topology cache hit: generation=%ld", topology->generation);
    } else {
        buckets_debug("Topology cache miss");
    }
    
    return topology;
}

int buckets_topology_cache_set(buckets_cluster_topology_t *topology)
{
    if (!g_topology_cache.initialized) {
        buckets_warn("Topology cache not initialized");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (!topology) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_rwlock_wrlock(&g_topology_cache.lock);
    
    /* Free old topology if exists */
    if (g_topology_cache.topology) {
        buckets_topology_free(g_topology_cache.topology);
    }
    
    /* Cache takes ownership */
    g_topology_cache.topology = topology;
    
    pthread_rwlock_unlock(&g_topology_cache.lock);
    
    buckets_info("Topology cached: generation=%ld, deployment_id=%s",
                 topology->generation, topology->deployment_id);
    
    return BUCKETS_OK;
}

void buckets_topology_cache_invalidate(void)
{
    if (!g_topology_cache.initialized) {
        return;
    }
    
    pthread_rwlock_wrlock(&g_topology_cache.lock);
    
    if (g_topology_cache.topology) {
        buckets_topology_free(g_topology_cache.topology);
        g_topology_cache.topology = NULL;
        buckets_info("Topology cache invalidated");
    }
    
    pthread_rwlock_unlock(&g_topology_cache.lock);
}
