/**
 * Location Registry API
 * 
 * Self-hosted registry for tracking object locations in the cluster.
 * Provides fast lookups (<5ms) with LRU caching and batch operations.
 * 
 * Design:
 * - Storage: Self-hosted on Buckets (.buckets-registry bucket)
 * - Caching: LRU cache (1M entries, 5-min TTL)
 * - Durability: Leverages existing erasure coding
 * - No external dependencies
 */

#ifndef BUCKETS_REGISTRY_H
#define BUCKETS_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "buckets.h"

/* Registry constants */
#define BUCKETS_REGISTRY_BUCKET ".buckets-registry"
#define BUCKETS_REGISTRY_CACHE_SIZE (1000000)  /* 1M entries */
#define BUCKETS_REGISTRY_CACHE_TTL (300)       /* 5 minutes */
#define BUCKETS_REGISTRY_MAX_DISKS (16)        /* Max disks per set */

/**
 * Object location information
 * 
 * Tracks where an object version is physically stored in the cluster.
 */
typedef struct {
    /* Object identity */
    char *bucket;           /* Bucket name */
    char *object;           /* Object key */
    char *version_id;       /* Version ID (UUID) */
    
    /* Physical location */
    u32 pool_idx;           /* Pool index */
    u32 set_idx;            /* Erasure set index within pool */
    u32 disk_count;         /* Number of disks in set */
    u32 disk_idxs[BUCKETS_REGISTRY_MAX_DISKS];  /* Disk indices */
    
    /* Metadata */
    u64 generation;         /* Topology generation number */
    time_t mod_time;        /* Last modification time */
    size_t size;            /* Object size in bytes */
} buckets_object_location_t;

/**
 * Registry key for lookups
 */
typedef struct {
    char *bucket;           /* Bucket name */
    char *object;           /* Object key */
    char *version_id;       /* Version ID (NULL for latest) */
} buckets_registry_key_t;

/**
 * Registry cache statistics
 */
typedef struct {
    u64 hits;               /* Cache hits */
    u64 misses;             /* Cache misses */
    u64 evictions;          /* Entries evicted */
    u64 total_entries;      /* Current number of entries */
    double hit_rate;        /* Hit rate percentage (0-100) */
} buckets_registry_stats_t;

/**
 * Registry configuration
 */
typedef struct {
    u32 cache_size;         /* LRU cache size (default: 1M) */
    u32 cache_ttl_seconds;  /* Cache TTL (default: 300s) */
    bool enable_cache;      /* Enable caching (default: true) */
} buckets_registry_config_t;

/* ===== Registry Lifecycle ===== */

/**
 * Initialize location registry
 * 
 * @param config Registry configuration (NULL for defaults)
 * @return 0 on success, -1 on error
 */
int buckets_registry_init(const buckets_registry_config_t *config);

/**
 * Cleanup location registry
 */
void buckets_registry_cleanup(void);

/**
 * Get current registry configuration
 */
const buckets_registry_config_t* buckets_registry_get_config(void);

/* ===== Core Operations ===== */

/**
 * Record object location
 * 
 * Atomically records where an object version is stored.
 * Writes to both registry storage and cache.
 * 
 * @param location Object location information
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   buckets_object_location_t loc = {
 *       .bucket = "mybucket",
 *       .object = "photos/vacation.jpg",
 *       .version_id = "3fa9b0a8-...",
 *       .pool_idx = 0,
 *       .set_idx = 2,
 *       .disk_count = 12,
 *       .generation = 1,
 *       .mod_time = time(NULL),
 *       .size = 2048576
 *   };
 *   buckets_registry_record(&loc);
 */
int buckets_registry_record(const buckets_object_location_t *location);

/**
 * Lookup object location
 * 
 * Searches cache first, then registry storage if cache miss.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param version_id Version ID (NULL for latest version)
 * @param location Output location (caller must free with buckets_registry_location_free)
 * @return 0 on success, -1 on error (not found or I/O error)
 * 
 * Example:
 *   buckets_object_location_t *loc;
 *   if (buckets_registry_lookup("mybucket", "photos/vacation.jpg", NULL, &loc) == 0) {
 *       printf("Object is in pool %u, set %u\n", loc->pool_idx, loc->set_idx);
 *       buckets_registry_location_free(loc);
 *   }
 */
int buckets_registry_lookup(const char *bucket, const char *object,
                            const char *version_id,
                            buckets_object_location_t **location);

/**
 * Update object location
 * 
 * Atomically updates location information (used during migration).
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param version_id Version ID
 * @param location New location information
 * @return 0 on success, -1 on error
 */
int buckets_registry_update(const char *bucket, const char *object,
                            const char *version_id,
                            const buckets_object_location_t *location);

/**
 * Delete object location
 * 
 * Removes location record from registry and cache.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param version_id Version ID (NULL to delete all versions)
 * @return 0 on success, -1 on error
 */
int buckets_registry_delete(const char *bucket, const char *object,
                            const char *version_id);

/* ===== Batch Operations ===== */

/**
 * Record multiple object locations atomically
 * 
 * @param locations Array of object locations
 * @param count Number of locations
 * @return Number of successfully recorded locations, -1 on error
 */
int buckets_registry_record_batch(const buckets_object_location_t *locations,
                                   size_t count);

/**
 * Lookup multiple object locations
 * 
 * @param keys Array of registry keys
 * @param count Number of keys
 * @param locations Output array (caller must free each entry and array)
 * @return Number of successfully looked up locations, -1 on error
 */
int buckets_registry_lookup_batch(const buckets_registry_key_t *keys,
                                   size_t count,
                                   buckets_object_location_t ***locations);

/* ===== Cache Management ===== */

/**
 * Invalidate cache entry
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param version_id Version ID (NULL for all versions)
 * @return 0 on success, -1 on error
 */
int buckets_registry_cache_invalidate(const char *bucket, const char *object,
                                       const char *version_id);

/**
 * Clear entire cache
 */
void buckets_registry_cache_clear(void);

/**
 * Get cache statistics
 * 
 * @param stats Output statistics structure
 * @return 0 on success, -1 on error
 */
int buckets_registry_get_stats(buckets_registry_stats_t *stats);

/* ===== Memory Management ===== */

/**
 * Allocate and initialize object location
 * 
 * @return Allocated location (caller must free with buckets_registry_location_free)
 */
buckets_object_location_t* buckets_registry_location_new(void);

/**
 * Free object location
 * 
 * @param location Object location to free (can be NULL)
 */
void buckets_registry_location_free(buckets_object_location_t *location);

/**
 * Clone object location
 * 
 * @param src Source location
 * @return Cloned location (caller must free with buckets_registry_location_free)
 */
buckets_object_location_t* buckets_registry_location_clone(
    const buckets_object_location_t *src);

/* ===== Serialization ===== */

/**
 * Serialize object location to JSON
 * 
 * @param location Object location
 * @return JSON string (caller must free with buckets_free), NULL on error
 */
char* buckets_registry_location_to_json(const buckets_object_location_t *location);

/**
 * Deserialize object location from JSON
 * 
 * @param json JSON string
 * @return Object location (caller must free with buckets_registry_location_free),
 *         NULL on error
 */
buckets_object_location_t* buckets_registry_location_from_json(const char *json);

/* ===== Utilities ===== */

/**
 * Build registry key for storage
 * 
 * Format: {bucket}/{object}/{version-id}.json
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param version_id Version ID
 * @return Registry key string (caller must free with buckets_free), NULL on error
 */
char* buckets_registry_build_key(const char *bucket, const char *object,
                                  const char *version_id);

/**
 * Parse registry key
 * 
 * @param key Registry key string
 * @param bucket Output bucket name (caller must free)
 * @param object Output object key (caller must free)
 * @param version_id Output version ID (caller must free)
 * @return 0 on success, -1 on error
 */
int buckets_registry_parse_key(const char *key, char **bucket, char **object,
                                char **version_id);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_REGISTRY_H */
