/**
 * Buckets Cache Management
 * 
 * Thread-safe global caches for format and topology metadata
 */

#ifndef BUCKETS_CACHE_H
#define BUCKETS_CACHE_H

#include <pthread.h>
#include "buckets.h"
#include "buckets_cluster.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Format Cache ===== */

/**
 * Initialize format cache (called from buckets_init)
 */
void buckets_format_cache_init(void);

/**
 * Cleanup format cache (called from buckets_cleanup)
 */
void buckets_format_cache_cleanup(void);

/**
 * Get format from cache
 * 
 * @return Format pointer (do NOT free, cache owns it), or NULL if not cached
 */
buckets_format_t* buckets_format_cache_get(void);

/**
 * Set format in cache (makes a copy)
 * 
 * @param format Format to cache (will be cloned)
 * @return BUCKETS_OK on success
 */
int buckets_format_cache_set(buckets_format_t *format);

/**
 * Invalidate format cache
 */
void buckets_format_cache_invalidate(void);

/* ===== Topology Cache ===== */

/**
 * Initialize topology cache (called from buckets_init)
 */
void buckets_topology_cache_init(void);

/**
 * Cleanup topology cache (called from buckets_cleanup)
 */
void buckets_topology_cache_cleanup(void);

/**
 * Get topology from cache
 * 
 * @return Topology pointer (do NOT free, cache owns it), or NULL if not cached
 */
buckets_cluster_topology_t* buckets_topology_cache_get(void);

/**
 * Set topology in cache (takes ownership)
 * 
 * @param topology Topology to cache (ownership transferred)
 * @return BUCKETS_OK on success
 */
int buckets_topology_cache_set(buckets_cluster_topology_t *topology);

/**
 * Invalidate topology cache
 */
void buckets_topology_cache_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_CACHE_H */
