/**
 * Metadata Caching Layer
 * 
 * LRU cache for xl.meta to reduce disk I/O for frequently accessed objects.
 * Thread-safe with read-write locks.
 */

/* Disable format-truncation warnings */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_hash.h"

/* Cache configuration */
#define CACHE_SIZE 10000         /* Max cached entries */
#define CACHE_TTL_SECONDS 300    /* 5 minutes */

/**
 * Cache entry
 */
typedef struct cache_entry {
    char *key;                     /* "bucket/object/versionId" */
    buckets_xl_meta_t *meta;       /* Cached metadata (owned by cache) */
    time_t timestamp;              /* Last access time */
    struct cache_entry *next;      /* Hash chain */
    struct cache_entry *lru_prev;  /* LRU list */
    struct cache_entry *lru_next;  /* LRU list */
} cache_entry_t;

/**
 * Metadata cache
 */
typedef struct {
    cache_entry_t **hash_table;    /* Hash table (open chaining) */
    cache_entry_t *lru_head;       /* LRU list head (most recent) */
    cache_entry_t *lru_tail;       /* LRU list tail (least recent) */
    u32 table_size;                /* Hash table size */
    u32 count;                     /* Current entry count */
    u32 max_size;                  /* Maximum entries */
    u32 ttl_seconds;               /* Time-to-live in seconds */
    pthread_rwlock_t lock;         /* Reader-writer lock */
    
    /* Statistics */
    u64 hits;
    u64 misses;
    u64 evictions;
} metadata_cache_t;

/* Global cache instance */
static metadata_cache_t *g_metadata_cache = NULL;

/**
 * Compute cache key hash
 */
static u32 cache_key_hash(const char *key, u32 table_size)
{
    u64 hash = buckets_xxhash64(0, key, strlen(key));
    return (u32)(hash % table_size);
}

/**
 * Create cache key from bucket, object, and optional version ID
 */
static char* create_cache_key(const char *bucket, const char *object,
                              const char *versionId)
{
    char key[PATH_MAX];
    if (versionId) {
        snprintf(key, sizeof(key), "%s/%s/%s", bucket, object, versionId);
    } else {
        snprintf(key, sizeof(key), "%s/%s", bucket, object);
    }
    return buckets_strdup(key);
}

/**
 * Deep clone xl.meta for caching
 * 
 * Note: Shallow copy of pointers won't work - cache needs owned data
 */
static buckets_xl_meta_t* clone_xl_meta(const buckets_xl_meta_t *src)
{
    if (!src) return NULL;
    
    buckets_xl_meta_t *dst = buckets_malloc(sizeof(buckets_xl_meta_t));
    memset(dst, 0, sizeof(buckets_xl_meta_t));
    
    /* Copy simple fields */
    dst->version = src->version;
    strcpy(dst->format, src->format);
    dst->stat = src->stat;
    dst->erasure.data = src->erasure.data;
    dst->erasure.parity = src->erasure.parity;
    dst->erasure.blockSize = src->erasure.blockSize;
    dst->erasure.index = src->erasure.index;
    strcpy(dst->erasure.algorithm, src->erasure.algorithm);
    
    /* Clone strings */
    if (src->meta.content_type) {
        dst->meta.content_type = buckets_strdup(src->meta.content_type);
    }
    if (src->meta.etag) {
        dst->meta.etag = buckets_strdup(src->meta.etag);
    }
    if (src->inline_data) {
        dst->inline_data = buckets_strdup(src->inline_data);
    }
    if (src->versioning.versionId) {
        dst->versioning.versionId = buckets_strdup(src->versioning.versionId);
    }
    dst->versioning.isLatest = src->versioning.isLatest;
    dst->versioning.isDeleteMarker = src->versioning.isDeleteMarker;
    
    /* Clone arrays */
    if (src->erasure.distribution && (src->erasure.data + src->erasure.parity) > 0) {
        u32 count = src->erasure.data + src->erasure.parity;
        dst->erasure.distribution = buckets_malloc(count * sizeof(u32));
        memcpy(dst->erasure.distribution, src->erasure.distribution, 
               count * sizeof(u32));
    }
    
    if (src->erasure.checksums && (src->erasure.data + src->erasure.parity) > 0) {
        u32 count = src->erasure.data + src->erasure.parity;
        dst->erasure.checksums = buckets_malloc(count * sizeof(buckets_checksum_t));
        memcpy(dst->erasure.checksums, src->erasure.checksums,
               count * sizeof(buckets_checksum_t));
    }
    
    return dst;
}

/**
 * Remove entry from LRU list
 */
static void lru_remove(metadata_cache_t *cache, cache_entry_t *entry)
{
    if (entry->lru_prev) {
        entry->lru_prev->lru_next = entry->lru_next;
    } else {
        cache->lru_head = entry->lru_next;
    }
    
    if (entry->lru_next) {
        entry->lru_next->lru_prev = entry->lru_prev;
    } else {
        cache->lru_tail = entry->lru_prev;
    }
    
    entry->lru_prev = NULL;
    entry->lru_next = NULL;
}

/**
 * Add entry to front of LRU list (most recent)
 */
static void lru_push_front(metadata_cache_t *cache, cache_entry_t *entry)
{
    entry->lru_prev = NULL;
    entry->lru_next = cache->lru_head;
    
    if (cache->lru_head) {
        cache->lru_head->lru_prev = entry;
    } else {
        cache->lru_tail = entry;
    }
    
    cache->lru_head = entry;
}

/**
 * Move entry to front of LRU list (mark as recently used)
 */
static void lru_touch(metadata_cache_t *cache, cache_entry_t *entry)
{
    if (entry == cache->lru_head) {
        return;  /* Already at front */
    }
    
    lru_remove(cache, entry);
    lru_push_front(cache, entry);
}

/**
 * Evict least recently used entry
 */
static void evict_lru_entry(metadata_cache_t *cache)
{
    if (!cache->lru_tail) {
        return;  /* Cache empty */
    }
    
    cache_entry_t *victim = cache->lru_tail;
    
    /* Remove from LRU list */
    lru_remove(cache, victim);
    
    /* Remove from hash table */
    u32 hash = cache_key_hash(victim->key, cache->table_size);
    cache_entry_t **slot = &cache->hash_table[hash];
    cache_entry_t *prev = NULL;
    cache_entry_t *curr = *slot;
    
    while (curr) {
        if (curr == victim) {
            if (prev) {
                prev->next = curr->next;
            } else {
                *slot = curr->next;
            }
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    
    /* Free entry */
    buckets_free(victim->key);
    buckets_xl_meta_free(victim->meta);
    buckets_free(victim->meta);
    buckets_free(victim);
    
    cache->count--;
    cache->evictions++;
}

/**
 * Initialize metadata cache
 * 
 * @param max_size Maximum number of cached entries
 * @param ttl_seconds Time-to-live for entries (0 = no expiration)
 * @return 0 on success, -1 on error
 */
int buckets_metadata_cache_init(u32 max_size, u32 ttl_seconds)
{
    if (g_metadata_cache) {
        buckets_warn("Metadata cache already initialized");
        return 0;
    }
    
    g_metadata_cache = buckets_malloc(sizeof(metadata_cache_t));
    memset(g_metadata_cache, 0, sizeof(metadata_cache_t));
    
    /* Initialize configuration */
    g_metadata_cache->max_size = max_size > 0 ? max_size : CACHE_SIZE;
    g_metadata_cache->ttl_seconds = ttl_seconds > 0 ? ttl_seconds : CACHE_TTL_SECONDS;
    g_metadata_cache->table_size = g_metadata_cache->max_size;  /* 1:1 ratio */
    
    /* Allocate hash table */
    g_metadata_cache->hash_table = buckets_calloc(g_metadata_cache->table_size,
                                                   sizeof(cache_entry_t*));
    
    /* Initialize rwlock */
    pthread_rwlock_init(&g_metadata_cache->lock, NULL);
    
    buckets_info("Metadata cache initialized: max_size=%u, ttl=%us",
                 g_metadata_cache->max_size, g_metadata_cache->ttl_seconds);
    
    return 0;
}

/**
 * Cleanup metadata cache
 */
void buckets_metadata_cache_cleanup(void)
{
    if (!g_metadata_cache) {
        return;
    }
    
    pthread_rwlock_wrlock(&g_metadata_cache->lock);
    
    /* Free all entries */
    for (u32 i = 0; i < g_metadata_cache->table_size; i++) {
        cache_entry_t *entry = g_metadata_cache->hash_table[i];
        while (entry) {
            cache_entry_t *next = entry->next;
            buckets_free(entry->key);
            buckets_xl_meta_free(entry->meta);
            buckets_free(entry->meta);
            buckets_free(entry);
            entry = next;
        }
    }
    
    buckets_free(g_metadata_cache->hash_table);
    
    pthread_rwlock_unlock(&g_metadata_cache->lock);
    pthread_rwlock_destroy(&g_metadata_cache->lock);
    
    buckets_info("Metadata cache cleanup: hits=%llu, misses=%llu, evictions=%llu",
                 (unsigned long long)g_metadata_cache->hits,
                 (unsigned long long)g_metadata_cache->misses,
                 (unsigned long long)g_metadata_cache->evictions);
    
    buckets_free(g_metadata_cache);
    g_metadata_cache = NULL;
}

/**
 * Get metadata from cache
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID (NULL for latest)
 * @param meta Output metadata (caller must free with buckets_xl_meta_free if non-NULL)
 * @return 0 on cache hit, -1 on cache miss
 */
int buckets_metadata_cache_get(const char *bucket, const char *object,
                                const char *versionId,
                                buckets_xl_meta_t *meta)
{
    if (!g_metadata_cache || !bucket || !object || !meta) {
        return -1;
    }
    
    char *key = create_cache_key(bucket, object, versionId);
    u32 hash = cache_key_hash(key, g_metadata_cache->table_size);
    
    pthread_rwlock_rdlock(&g_metadata_cache->lock);
    
    /* Search hash chain */
    cache_entry_t *entry = g_metadata_cache->hash_table[hash];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            /* Check TTL */
            time_t now = time(NULL);
            if (g_metadata_cache->ttl_seconds > 0 &&
                (now - entry->timestamp) > (time_t)g_metadata_cache->ttl_seconds) {
                /* Expired */
                pthread_rwlock_unlock(&g_metadata_cache->lock);
                buckets_free(key);
                g_metadata_cache->misses++;
                return -1;
            }
            
            /* Cache hit - clone metadata */
            *meta = *clone_xl_meta(entry->meta);
            
            pthread_rwlock_unlock(&g_metadata_cache->lock);
            
            /* Update LRU (need write lock for this) */
            pthread_rwlock_wrlock(&g_metadata_cache->lock);
            lru_touch(g_metadata_cache, entry);
            entry->timestamp = time(NULL);
            pthread_rwlock_unlock(&g_metadata_cache->lock);
            
            buckets_free(key);
            g_metadata_cache->hits++;
            return 0;
        }
        entry = entry->next;
    }
    
    pthread_rwlock_unlock(&g_metadata_cache->lock);
    
    buckets_free(key);
    g_metadata_cache->misses++;
    return -1;
}

/**
 * Put metadata into cache
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID (NULL for latest)
 * @param meta Metadata to cache (will be cloned)
 * @return 0 on success, -1 on error
 */
int buckets_metadata_cache_put(const char *bucket, const char *object,
                                const char *versionId,
                                const buckets_xl_meta_t *meta)
{
    if (!g_metadata_cache || !bucket || !object || !meta) {
        return -1;
    }
    
    char *key = create_cache_key(bucket, object, versionId);
    u32 hash = cache_key_hash(key, g_metadata_cache->table_size);
    
    pthread_rwlock_wrlock(&g_metadata_cache->lock);
    
    /* Check if already cached (update existing) */
    cache_entry_t *entry = g_metadata_cache->hash_table[hash];
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            /* Update existing entry */
            buckets_xl_meta_free(entry->meta);
            buckets_free(entry->meta);
            entry->meta = clone_xl_meta(meta);
            entry->timestamp = time(NULL);
            lru_touch(g_metadata_cache, entry);
            
            pthread_rwlock_unlock(&g_metadata_cache->lock);
            buckets_free(key);
            return 0;
        }
        entry = entry->next;
    }
    
    /* Evict if cache is full */
    if (g_metadata_cache->count >= g_metadata_cache->max_size) {
        evict_lru_entry(g_metadata_cache);
    }
    
    /* Create new entry */
    cache_entry_t *new_entry = buckets_malloc(sizeof(cache_entry_t));
    new_entry->key = key;
    new_entry->meta = clone_xl_meta(meta);
    new_entry->timestamp = time(NULL);
    new_entry->lru_prev = NULL;
    new_entry->lru_next = NULL;
    
    /* Insert into hash table */
    new_entry->next = g_metadata_cache->hash_table[hash];
    g_metadata_cache->hash_table[hash] = new_entry;
    
    /* Add to LRU list */
    lru_push_front(g_metadata_cache, new_entry);
    
    g_metadata_cache->count++;
    
    pthread_rwlock_unlock(&g_metadata_cache->lock);
    
    buckets_debug("Cached metadata: %s (count=%u)", key, g_metadata_cache->count);
    return 0;
}

/**
 * Invalidate cache entry
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID (NULL for latest)
 * @return 0 on success, -1 if not found
 */
int buckets_metadata_cache_invalidate(const char *bucket, const char *object,
                                       const char *versionId)
{
    if (!g_metadata_cache || !bucket || !object) {
        return -1;
    }
    
    char *key = create_cache_key(bucket, object, versionId);
    u32 hash = cache_key_hash(key, g_metadata_cache->table_size);
    
    pthread_rwlock_wrlock(&g_metadata_cache->lock);
    
    /* Find and remove entry */
    cache_entry_t **slot = &g_metadata_cache->hash_table[hash];
    cache_entry_t *prev = NULL;
    cache_entry_t *curr = *slot;
    
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            /* Remove from hash chain */
            if (prev) {
                prev->next = curr->next;
            } else {
                *slot = curr->next;
            }
            
            /* Remove from LRU list */
            lru_remove(g_metadata_cache, curr);
            
            /* Free entry */
            buckets_free(curr->key);
            buckets_xl_meta_free(curr->meta);
            buckets_free(curr->meta);
            buckets_free(curr);
            
            g_metadata_cache->count--;
            
            pthread_rwlock_unlock(&g_metadata_cache->lock);
            buckets_free(key);
            
            buckets_debug("Invalidated cache entry: %s", key);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    
    pthread_rwlock_unlock(&g_metadata_cache->lock);
    buckets_free(key);
    
    return -1;  /* Not found */
}

/**
 * Get cache statistics
 * 
 * @param hits Output hit count
 * @param misses Output miss count
 * @param evictions Output eviction count
 * @param count Output current entry count
 */
void buckets_metadata_cache_stats(u64 *hits, u64 *misses, u64 *evictions, u32 *count)
{
    if (!g_metadata_cache) {
        if (hits) *hits = 0;
        if (misses) *misses = 0;
        if (evictions) *evictions = 0;
        if (count) *count = 0;
        return;
    }
    
    pthread_rwlock_rdlock(&g_metadata_cache->lock);
    
    if (hits) *hits = g_metadata_cache->hits;
    if (misses) *misses = g_metadata_cache->misses;
    if (evictions) *evictions = g_metadata_cache->evictions;
    if (count) *count = g_metadata_cache->count;
    
    pthread_rwlock_unlock(&g_metadata_cache->lock);
}

#pragma GCC diagnostic pop
