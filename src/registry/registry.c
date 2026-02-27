/**
 * Location Registry Implementation
 * 
 * Self-hosted registry for tracking object locations using the storage layer.
 * Provides fast lookups with LRU caching and persistence to .buckets-registry bucket.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "buckets.h"
#include "buckets_registry.h"
#include "buckets_storage.h"
#include "buckets_json.h"
#include "buckets_hash.h"

/* Registry cache entry */
typedef struct registry_cache_entry {
    char *key;                              /* Cache key: bucket/object/version-id */
    buckets_object_location_t *location;    /* Cached location */
    time_t expiry;                          /* Expiry timestamp */
    struct registry_cache_entry *next;      /* Next in hash chain */
    struct registry_cache_entry *lru_prev;  /* LRU list prev */
    struct registry_cache_entry *lru_next;  /* LRU list next */
} registry_cache_entry_t;

/* Registry cache */
typedef struct {
    registry_cache_entry_t **buckets;   /* Hash table buckets */
    u32 bucket_count;                   /* Number of hash buckets */
    u32 entry_count;                    /* Current number of entries */
    u32 max_entries;                    /* Maximum entries (LRU size) */
    u32 ttl_seconds;                    /* TTL in seconds */
    
    /* LRU list (most recent at head) */
    registry_cache_entry_t *lru_head;
    registry_cache_entry_t *lru_tail;
    
    /* Statistics */
    u64 hits;
    u64 misses;
    u64 evictions;
    
    /* Thread safety */
    pthread_rwlock_t lock;
} registry_cache_t;

/* Global registry state */
static struct {
    buckets_registry_config_t config;
    registry_cache_t *cache;
    bool initialized;
    pthread_mutex_t init_lock;
} g_registry = {
    .initialized = false,
    .init_lock = PTHREAD_MUTEX_INITIALIZER
};

/* ========================================================================
 * Cache Implementation
 * ======================================================================== */

static u32 cache_hash(const char *key, u32 bucket_count)
{
    u64 hash = buckets_xxhash64(0, key, strlen(key));
    return (u32)(hash % bucket_count);
}

static registry_cache_t* cache_create(u32 max_entries, u32 ttl_seconds)
{
    registry_cache_t *cache = buckets_calloc(1, sizeof(registry_cache_t));
    if (!cache) {
        return NULL;
    }
    
    /* Use prime number for bucket count (roughly 10% of max entries) */
    cache->bucket_count = (max_entries / 10) | 1;  /* Ensure odd */
    cache->buckets = buckets_calloc(cache->bucket_count, sizeof(registry_cache_entry_t*));
    if (!cache->buckets) {
        buckets_free(cache);
        return NULL;
    }
    
    cache->max_entries = max_entries;
    cache->ttl_seconds = ttl_seconds;
    cache->entry_count = 0;
    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    cache->hits = 0;
    cache->misses = 0;
    cache->evictions = 0;
    
    if (pthread_rwlock_init(&cache->lock, NULL) != 0) {
        buckets_free(cache->buckets);
        buckets_free(cache);
        return NULL;
    }
    
    return cache;
}

static void cache_entry_free(registry_cache_entry_t *entry)
{
    if (!entry) {
        return;
    }
    
    buckets_free(entry->key);
    buckets_registry_location_free(entry->location);
    buckets_free(entry);
}

static void cache_lru_remove(registry_cache_t *cache, registry_cache_entry_t *entry)
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

static void cache_lru_add_head(registry_cache_t *cache, registry_cache_entry_t *entry)
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

static void cache_evict_lru(registry_cache_t *cache)
{
    if (!cache->lru_tail) {
        return;
    }
    
    registry_cache_entry_t *victim = cache->lru_tail;
    
    /* Remove from LRU list */
    cache_lru_remove(cache, victim);
    
    /* Remove from hash table */
    u32 bucket_idx = cache_hash(victim->key, cache->bucket_count);
    registry_cache_entry_t **curr = &cache->buckets[bucket_idx];
    
    while (*curr) {
        if (*curr == victim) {
            *curr = victim->next;
            break;
        }
        curr = &(*curr)->next;
    }
    
    cache->entry_count--;
    cache->evictions++;
    cache_entry_free(victim);
}

static buckets_object_location_t* cache_get(registry_cache_t *cache, const char *key)
{
    pthread_rwlock_rdlock(&cache->lock);
    
    u32 bucket_idx = cache_hash(key, cache->bucket_count);
    registry_cache_entry_t *entry = cache->buckets[bucket_idx];
    time_t now = time(NULL);
    
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            /* Check expiry */
            if (entry->expiry < now) {
                pthread_rwlock_unlock(&cache->lock);
                cache->misses++;
                return NULL;  /* Expired */
            }
            
            /* Cache hit */
            cache->hits++;
            buckets_object_location_t *result = buckets_registry_location_clone(entry->location);
            pthread_rwlock_unlock(&cache->lock);
            
            /* Move to head of LRU (upgrade to write lock) */
            pthread_rwlock_wrlock(&cache->lock);
            cache_lru_remove(cache, entry);
            cache_lru_add_head(cache, entry);
            pthread_rwlock_unlock(&cache->lock);
            
            return result;
        }
        entry = entry->next;
    }
    
    pthread_rwlock_unlock(&cache->lock);
    cache->misses++;
    return NULL;  /* Not found */
}

static int cache_put(registry_cache_t *cache, const char *key,
                     const buckets_object_location_t *location)
{
    pthread_rwlock_wrlock(&cache->lock);
    
    /* Check if entry exists and update it */
    u32 bucket_idx = cache_hash(key, cache->bucket_count);
    registry_cache_entry_t *entry = cache->buckets[bucket_idx];
    
    while (entry) {
        if (strcmp(entry->key, key) == 0) {
            /* Update existing entry */
            buckets_registry_location_free(entry->location);
            entry->location = buckets_registry_location_clone(location);
            entry->expiry = time(NULL) + cache->ttl_seconds;
            
            /* Move to head of LRU */
            cache_lru_remove(cache, entry);
            cache_lru_add_head(cache, entry);
            
            pthread_rwlock_unlock(&cache->lock);
            return 0;
        }
        entry = entry->next;
    }
    
    /* Evict LRU if cache is full */
    if (cache->entry_count >= cache->max_entries) {
        cache_evict_lru(cache);
    }
    
    /* Create new entry */
    entry = buckets_calloc(1, sizeof(registry_cache_entry_t));
    if (!entry) {
        pthread_rwlock_unlock(&cache->lock);
        return -1;
    }
    
    entry->key = buckets_strdup(key);
    entry->location = buckets_registry_location_clone(location);
    entry->expiry = time(NULL) + cache->ttl_seconds;
    
    /* Add to hash table */
    entry->next = cache->buckets[bucket_idx];
    cache->buckets[bucket_idx] = entry;
    
    /* Add to head of LRU */
    cache_lru_add_head(cache, entry);
    
    cache->entry_count++;
    
    pthread_rwlock_unlock(&cache->lock);
    return 0;
}

static int cache_invalidate(registry_cache_t *cache, const char *key)
{
    pthread_rwlock_wrlock(&cache->lock);
    
    u32 bucket_idx = cache_hash(key, cache->bucket_count);
    registry_cache_entry_t **curr = &cache->buckets[bucket_idx];
    
    while (*curr) {
        if (strcmp((*curr)->key, key) == 0) {
            registry_cache_entry_t *entry = *curr;
            *curr = entry->next;
            
            cache_lru_remove(cache, entry);
            cache->entry_count--;
            cache_entry_free(entry);
            
            pthread_rwlock_unlock(&cache->lock);
            return 0;
        }
        curr = &(*curr)->next;
    }
    
    pthread_rwlock_unlock(&cache->lock);
    return -1;  /* Not found */
}

static void cache_clear(registry_cache_t *cache)
{
    pthread_rwlock_wrlock(&cache->lock);
    
    for (u32 i = 0; i < cache->bucket_count; i++) {
        registry_cache_entry_t *entry = cache->buckets[i];
        while (entry) {
            registry_cache_entry_t *next = entry->next;
            cache_entry_free(entry);
            entry = next;
        }
        cache->buckets[i] = NULL;
    }
    
    cache->entry_count = 0;
    cache->lru_head = NULL;
    cache->lru_tail = NULL;
    
    pthread_rwlock_unlock(&cache->lock);
}

static void cache_destroy(registry_cache_t *cache)
{
    if (!cache) {
        return;
    }
    
    cache_clear(cache);
    pthread_rwlock_destroy(&cache->lock);
    buckets_free(cache->buckets);
    buckets_free(cache);
}

/* ========================================================================
 * Registry Initialization
 * ======================================================================== */

int buckets_registry_init(const buckets_registry_config_t *config)
{
    pthread_mutex_lock(&g_registry.init_lock);
    
    if (g_registry.initialized) {
        pthread_mutex_unlock(&g_registry.init_lock);
        buckets_warn("Registry already initialized");
        return 0;
    }
    
    /* Set default config */
    if (config) {
        g_registry.config = *config;
    } else {
        g_registry.config.cache_size = BUCKETS_REGISTRY_CACHE_SIZE;
        g_registry.config.cache_ttl_seconds = BUCKETS_REGISTRY_CACHE_TTL;
        g_registry.config.enable_cache = true;
    }
    
    /* Initialize cache */
    if (g_registry.config.enable_cache) {
        g_registry.cache = cache_create(g_registry.config.cache_size,
                                        g_registry.config.cache_ttl_seconds);
        if (!g_registry.cache) {
            pthread_mutex_unlock(&g_registry.init_lock);
            buckets_error("Failed to create registry cache");
            return -1;
        }
    }
    
    g_registry.initialized = true;
    pthread_mutex_unlock(&g_registry.init_lock);
    
    buckets_info("Registry initialized (cache_size=%u, ttl=%u seconds)",
                 g_registry.config.cache_size, g_registry.config.cache_ttl_seconds);
    
    return 0;
}

void buckets_registry_cleanup(void)
{
    pthread_mutex_lock(&g_registry.init_lock);
    
    if (!g_registry.initialized) {
        pthread_mutex_unlock(&g_registry.init_lock);
        return;
    }
    
    if (g_registry.cache) {
        cache_destroy(g_registry.cache);
        g_registry.cache = NULL;
    }
    
    g_registry.initialized = false;
    pthread_mutex_unlock(&g_registry.init_lock);
    
    buckets_info("Registry cleanup complete");
}

const buckets_registry_config_t* buckets_registry_get_config(void)
{
    return &g_registry.config;
}

/* ========================================================================
 * Serialization
 * ======================================================================== */

char* buckets_registry_location_to_json(const buckets_object_location_t *location)
{
    if (!location) {
        return NULL;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    
    cJSON_AddStringToObject(root, "bucket", location->bucket);
    cJSON_AddStringToObject(root, "object", location->object);
    cJSON_AddStringToObject(root, "version_id", location->version_id);
    cJSON_AddNumberToObject(root, "pool_idx", location->pool_idx);
    cJSON_AddNumberToObject(root, "set_idx", location->set_idx);
    cJSON_AddNumberToObject(root, "disk_count", location->disk_count);
    
    /* Disk indices array */
    cJSON *disks = cJSON_CreateArray();
    for (u32 i = 0; i < location->disk_count; i++) {
        cJSON_AddItemToArray(disks, cJSON_CreateNumber(location->disk_idxs[i]));
    }
    cJSON_AddItemToObject(root, "disk_idxs", disks);
    
    cJSON_AddNumberToObject(root, "generation", location->generation);
    cJSON_AddNumberToObject(root, "mod_time", (double)location->mod_time);
    cJSON_AddNumberToObject(root, "size", (double)location->size);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    return json_str;
}

buckets_object_location_t* buckets_registry_location_from_json(const char *json)
{
    if (!json) {
        return NULL;
    }
    
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        buckets_error("Failed to parse registry JSON");
        return NULL;
    }
    
    buckets_object_location_t *location = buckets_registry_location_new();
    if (!location) {
        cJSON_Delete(root);
        return NULL;
    }
    
    /* Parse fields */
    location->bucket = buckets_strdup(buckets_json_get_string(root, "bucket", ""));
    location->object = buckets_strdup(buckets_json_get_string(root, "object", ""));
    location->version_id = buckets_strdup(buckets_json_get_string(root, "version_id", ""));
    location->pool_idx = buckets_json_get_int(root, "pool_idx", 0);
    location->set_idx = buckets_json_get_int(root, "set_idx", 0);
    location->disk_count = buckets_json_get_int(root, "disk_count", 0);
    
    /* Parse disk indices array */
    cJSON *disks = cJSON_GetObjectItem(root, "disk_idxs");
    if (cJSON_IsArray(disks)) {
        int count = cJSON_GetArraySize(disks);
        if (count > BUCKETS_REGISTRY_MAX_DISKS) {
            count = BUCKETS_REGISTRY_MAX_DISKS;
        }
        
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(disks, i);
            if (cJSON_IsNumber(item)) {
                location->disk_idxs[i] = (u32)item->valueint;
            }
        }
    }
    
    location->generation = buckets_json_get_int(root, "generation", 0);
    location->mod_time = (time_t)buckets_json_get_int(root, "mod_time", 0);
    location->size = (size_t)buckets_json_get_int(root, "size", 0);
    
    cJSON_Delete(root);
    return location;
}

/* ========================================================================
 * Memory Management
 * ======================================================================== */

buckets_object_location_t* buckets_registry_location_new(void)
{
    buckets_object_location_t *location = buckets_calloc(1, sizeof(buckets_object_location_t));
    return location;
}

void buckets_registry_location_free(buckets_object_location_t *location)
{
    if (!location) {
        return;
    }
    
    buckets_free(location->bucket);
    buckets_free(location->object);
    buckets_free(location->version_id);
    buckets_free(location);
}

buckets_object_location_t* buckets_registry_location_clone(const buckets_object_location_t *src)
{
    if (!src) {
        return NULL;
    }
    
    buckets_object_location_t *dst = buckets_registry_location_new();
    if (!dst) {
        return NULL;
    }
    
    dst->bucket = buckets_strdup(src->bucket);
    dst->object = buckets_strdup(src->object);
    dst->version_id = buckets_strdup(src->version_id);
    dst->pool_idx = src->pool_idx;
    dst->set_idx = src->set_idx;
    dst->disk_count = src->disk_count;
    memcpy(dst->disk_idxs, src->disk_idxs, sizeof(src->disk_idxs));
    dst->generation = src->generation;
    dst->mod_time = src->mod_time;
    dst->size = src->size;
    
    return dst;
}

/* ========================================================================
 * Utilities
 * ======================================================================== */

char* buckets_registry_build_key(const char *bucket, const char *object,
                                  const char *version_id)
{
    if (!bucket || !object || !version_id) {
        return NULL;
    }
    
    /* Format: bucket/object/version-id */
    size_t len = strlen(bucket) + strlen(object) + strlen(version_id) + 3;
    char *key = buckets_malloc(len);
    if (!key) {
        return NULL;
    }
    
    snprintf(key, len, "%s/%s/%s", bucket, object, version_id);
    return key;
}

int buckets_registry_parse_key(const char *key, char **bucket, char **object,
                                char **version_id)
{
    if (!key || !bucket || !object || !version_id) {
        return -1;
    }
    
    /* Find first slash */
    const char *slash1 = strchr(key, '/');
    if (!slash1) {
        return -1;
    }
    
    /* Find second slash */
    const char *slash2 = strchr(slash1 + 1, '/');
    if (!slash2) {
        return -1;
    }
    
    /* Extract parts */
    size_t bucket_len = slash1 - key;
    size_t object_len = slash2 - slash1 - 1;
    size_t version_len = strlen(slash2 + 1);
    
    *bucket = buckets_malloc(bucket_len + 1);
    *object = buckets_malloc(object_len + 1);
    *version_id = buckets_malloc(version_len + 1);
    
    if (!*bucket || !*object || !*version_id) {
        buckets_free(*bucket);
        buckets_free(*object);
        buckets_free(*version_id);
        return -1;
    }
    
    memcpy(*bucket, key, bucket_len);
    (*bucket)[bucket_len] = '\0';
    
    memcpy(*object, slash1 + 1, object_len);
    (*object)[object_len] = '\0';
    
    memcpy(*version_id, slash2 + 1, version_len);
    (*version_id)[version_len] = '\0';
    
    return 0;
}

/* ========================================================================
 * Core Operations (to be continued in next message...)
 * ======================================================================== */

int buckets_registry_record(const buckets_object_location_t *location)
{
    if (!g_registry.initialized) {
        buckets_error("Registry not initialized");
        return -1;
    }
    
    if (!location || !location->bucket || !location->object || !location->version_id) {
        buckets_error("Invalid location");
        return -1;
    }
    
    /* Build registry key */
    char *key = buckets_registry_build_key(location->bucket, location->object,
                                            location->version_id);
    if (!key) {
        return -1;
    }
    
    /* Serialize location to JSON */
    char *json = buckets_registry_location_to_json(location);
    if (!json) {
        buckets_free(key);
        return -1;
    }
    
    /* Write to storage (.buckets-registry bucket) */
    /* Storage format: bucket/object/version-id.json */
    char object_key[1024];
    snprintf(object_key, sizeof(object_key), "%s/%s/%s.json",
             location->bucket, location->object, location->version_id);
    
    int result = buckets_put_object(BUCKETS_REGISTRY_BUCKET, object_key,
                                     json, strlen(json), "application/json");
    
    if (result != 0) {
        buckets_error("Failed to write registry entry to storage");
        buckets_free(key);
        buckets_free(json);
        return -1;
    }
    
    /* Update cache */
    if (g_registry.cache) {
        cache_put(g_registry.cache, key, location);
    }
    
    buckets_free(key);
    buckets_free(json);
    
    buckets_debug("Recorded location: %s/%s/%s -> pool=%u, set=%u",
                  location->bucket, location->object, location->version_id,
                  location->pool_idx, location->set_idx);
    
    return 0;
}

int buckets_registry_lookup(const char *bucket, const char *object,
                            const char *version_id,
                            buckets_object_location_t **location)
{
    if (!g_registry.initialized) {
        buckets_error("Registry not initialized");
        return -1;
    }
    
    if (!bucket || !object || !location) {
        buckets_error("Invalid parameters");
        return -1;
    }
    
    /* Use "latest" if version_id is NULL */
    const char *vid = version_id ? version_id : "latest";
    
    /* Build registry key */
    char *key = buckets_registry_build_key(bucket, object, vid);
    if (!key) {
        return -1;
    }
    
    /* Try cache first */
    if (g_registry.cache) {
        *location = cache_get(g_registry.cache, key);
        if (*location) {
            buckets_free(key);
            buckets_debug("Cache hit: %s/%s/%s", bucket, object, vid);
            return 0;
        }
    }
    
    /* Cache miss - fetch from storage */
    char object_key[1024];
    snprintf(object_key, sizeof(object_key), "%s/%s/%s.json", bucket, object, vid);
    
    size_t json_size = 0;
    void *json_data = NULL;
    int result = buckets_get_object(BUCKETS_REGISTRY_BUCKET, object_key, &json_data, &json_size);
    
    if (result != 0 || !json_data) {
        buckets_free(key);
        buckets_debug("Cache miss and storage miss: %s/%s/%s", bucket, object, vid);
        return -1;  /* Not found */
    }
    
    /* Deserialize from JSON */
    *location = buckets_registry_location_from_json((const char*)json_data);
    buckets_free(json_data);
    
    if (!*location) {
        buckets_free(key);
        buckets_error("Failed to deserialize registry entry");
        return -1;
    }
    
    /* Update cache for future lookups */
    if (g_registry.cache) {
        cache_put(g_registry.cache, key, *location);
    }
    
    buckets_free(key);
    buckets_debug("Cache miss, loaded from storage: %s/%s/%s", bucket, object, vid);
    return 0;
}

int buckets_registry_delete(const char *bucket, const char *object,
                            const char *version_id)
{
    if (!g_registry.initialized) {
        buckets_error("Registry not initialized");
        return -1;
    }
    
    if (!bucket || !object) {
        buckets_error("Invalid parameters");
        return -1;
    }
    
    const char *vid = version_id ? version_id : "latest";
    char *key = buckets_registry_build_key(bucket, object, vid);
    if (!key) {
        return -1;
    }
    
    /* Delete from storage - use distributed delete to handle multi-disk layout */
    char object_key[1024];
    snprintf(object_key, sizeof(object_key), "%s/%s/%s.json", bucket, object, vid);
    
    extern int buckets_distributed_delete_object(const char *bucket, const char *object);
    int result = buckets_distributed_delete_object(BUCKETS_REGISTRY_BUCKET, object_key);
    if (result != 0) {
        buckets_warn("Failed to delete registry entry from storage (may not exist)");
    }
    
    /* Invalidate cache */
    if (g_registry.cache) {
        cache_invalidate(g_registry.cache, key);
    }
    
    buckets_free(key);
    buckets_debug("Deleted location: %s/%s/%s", bucket, object, vid);
    
    return 0;
}

int buckets_registry_cache_invalidate(const char *bucket, const char *object,
                                       const char *version_id)
{
    if (!g_registry.initialized || !g_registry.cache) {
        return -1;
    }
    
    const char *vid = version_id ? version_id : "latest";
    char *key = buckets_registry_build_key(bucket, object, vid);
    if (!key) {
        return -1;
    }
    
    int result = cache_invalidate(g_registry.cache, key);
    buckets_free(key);
    
    return result;
}

void buckets_registry_cache_clear(void)
{
    if (g_registry.cache) {
        cache_clear(g_registry.cache);
    }
}

int buckets_registry_get_stats(buckets_registry_stats_t *stats)
{
    if (!g_registry.initialized || !g_registry.cache || !stats) {
        return -1;
    }
    
    pthread_rwlock_rdlock(&g_registry.cache->lock);
    
    stats->hits = g_registry.cache->hits;
    stats->misses = g_registry.cache->misses;
    stats->evictions = g_registry.cache->evictions;
    stats->total_entries = g_registry.cache->entry_count;
    
    u64 total = stats->hits + stats->misses;
    stats->hit_rate = total > 0 ? (double)stats->hits * 100.0 / total : 0.0;
    
    pthread_rwlock_unlock(&g_registry.cache->lock);
    
    return 0;
}

/* ===== Batch Operations ===== */

int buckets_registry_record_batch(const buckets_object_location_t *locations,
                                   size_t count)
{
    if (!g_registry.initialized || !locations || count == 0) {
        return -1;
    }
    
    int success_count = 0;
    
    for (size_t i = 0; i < count; i++) {
        if (buckets_registry_record(&locations[i]) == 0) {
            success_count++;
        } else {
            buckets_warn("Batch record failed for item %zu: %s/%s", 
                        i, locations[i].bucket, locations[i].object);
        }
    }
    
    return success_count;
}

int buckets_registry_lookup_batch(const buckets_registry_key_t *keys,
                                   size_t count,
                                   buckets_object_location_t ***locations)
{
    if (!g_registry.initialized || !keys || count == 0 || !locations) {
        return -1;
    }
    
    /* Allocate output array */
    buckets_object_location_t **results = buckets_calloc(count, 
                                                          sizeof(buckets_object_location_t*));
    if (!results) {
        return -1;
    }
    
    int success_count = 0;
    
    for (size_t i = 0; i < count; i++) {
        buckets_object_location_t *loc = NULL;
        if (buckets_registry_lookup(keys[i].bucket, keys[i].object,
                                    keys[i].version_id, &loc) == 0) {
            results[i] = loc;
            success_count++;
        } else {
            results[i] = NULL;
        }
    }
    
    *locations = results;
    return success_count;
}

/* ===== Update Operation ===== */

int buckets_registry_update(const char *bucket, const char *object,
                            const char *version_id,
                            const buckets_object_location_t *location)
{
    if (!g_registry.initialized || !bucket || !object || !version_id || !location) {
        return -1;
    }
    
    /* Update is implemented as delete + record for atomicity */
    /* First, delete old location from cache (not storage yet) */
    char *cache_key = buckets_registry_build_key(bucket, object, version_id);
    if (!cache_key) {
        return -1;
    }
    
    cache_invalidate(g_registry.cache, cache_key);
    buckets_free(cache_key);
    
    /* Now record the new location (overwrites storage) */
    int result = buckets_registry_record(location);
    
    if (result == 0) {
        buckets_debug("Updated location for %s/%s (version %s)", 
                     bucket, object, version_id);
    }
    
    return result;
}

/* ===== Range Query Support ===== */

#include <dirent.h>
#include <sys/stat.h>

/* Helper to decode base64 inline data */
static char* decode_base64_inline(const char *encoded, size_t *out_len)
{
    if (!encoded) return NULL;
    
    size_t in_len = strlen(encoded);
    if (in_len == 0) return NULL;
    
    /* Base64 decode table */
    static const unsigned char decode_table[256] = {
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
        64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
        64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
    };
    
    /* Calculate output length */
    size_t padding = 0;
    if (in_len > 0 && encoded[in_len - 1] == '=') padding++;
    if (in_len > 1 && encoded[in_len - 2] == '=') padding++;
    
    size_t decoded_len = (in_len * 3) / 4 - padding;
    char *decoded = buckets_malloc(decoded_len + 1);
    if (!decoded) return NULL;
    
    size_t j = 0;
    unsigned int accum = 0;
    int bits = 0;
    
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)encoded[i];
        if (c == '=') break;
        
        unsigned char val = decode_table[c];
        if (val == 64) continue;  /* Skip invalid chars */
        
        accum = (accum << 6) | val;
        bits += 6;
        
        if (bits >= 8) {
            bits -= 8;
            decoded[j++] = (char)((accum >> bits) & 0xFF);
        }
    }
    
    decoded[j] = '\0';
    if (out_len) *out_len = j;
    return decoded;
}

/* Parse registry JSON into location struct */
static buckets_object_location_t* parse_registry_json(const char *json_str)
{
    cJSON *json = cJSON_Parse(json_str);
    if (!json) return NULL;
    
    buckets_object_location_t *loc = buckets_registry_location_new();
    if (!loc) {
        cJSON_Delete(json);
        return NULL;
    }
    
    /* Parse fields */
    cJSON *bucket = cJSON_GetObjectItem(json, "bucket");
    cJSON *object = cJSON_GetObjectItem(json, "object");
    cJSON *version_id = cJSON_GetObjectItem(json, "version_id");
    cJSON *pool_idx = cJSON_GetObjectItem(json, "pool_idx");
    cJSON *set_idx = cJSON_GetObjectItem(json, "set_idx");
    cJSON *disk_count = cJSON_GetObjectItem(json, "disk_count");
    cJSON *disk_idxs = cJSON_GetObjectItem(json, "disk_idxs");
    cJSON *generation = cJSON_GetObjectItem(json, "generation");
    cJSON *mod_time = cJSON_GetObjectItem(json, "mod_time");
    cJSON *size = cJSON_GetObjectItem(json, "size");
    
    if (bucket && cJSON_IsString(bucket)) {
        loc->bucket = buckets_strdup(bucket->valuestring);
    }
    if (object && cJSON_IsString(object)) {
        loc->object = buckets_strdup(object->valuestring);
    }
    if (version_id && cJSON_IsString(version_id)) {
        loc->version_id = buckets_strdup(version_id->valuestring);
    }
    if (pool_idx && cJSON_IsNumber(pool_idx)) {
        loc->pool_idx = (u32)pool_idx->valueint;
    }
    if (set_idx && cJSON_IsNumber(set_idx)) {
        loc->set_idx = (u32)set_idx->valueint;
    }
    if (disk_count && cJSON_IsNumber(disk_count)) {
        loc->disk_count = (u32)disk_count->valueint;
    }
    if (disk_idxs && cJSON_IsArray(disk_idxs)) {
        int arr_size = cJSON_GetArraySize(disk_idxs);
        for (int i = 0; i < arr_size && i < BUCKETS_REGISTRY_MAX_DISKS; i++) {
            cJSON *item = cJSON_GetArrayItem(disk_idxs, i);
            if (item && cJSON_IsNumber(item)) {
                loc->disk_idxs[i] = (u32)item->valueint;
            }
        }
    }
    if (generation && cJSON_IsNumber(generation)) {
        loc->generation = (u64)generation->valuedouble;
    }
    if (mod_time && cJSON_IsNumber(mod_time)) {
        loc->mod_time = (time_t)mod_time->valuedouble;
    }
    if (size && cJSON_IsNumber(size)) {
        loc->size = (size_t)size->valuedouble;
    }
    
    cJSON_Delete(json);
    return loc;
}

/**
 * List all objects in a bucket with optional prefix
 * 
 * Scans registry storage (all xl.meta files) to find registry entries
 * for the specified bucket.
 * 
 * @param bucket Bucket name to list
 * @param prefix Object key prefix (NULL for all objects)
 * @param max_keys Maximum number of keys to return (0 for unlimited)
 * @param locations Output array (caller must free)
 * @param count Output count of locations found
 * @return 0 on success, -1 on error
 */
int buckets_registry_list(const char *bucket, const char *prefix,
                          size_t max_keys,
                          buckets_object_location_t ***locations,
                          size_t *count)
{
    if (!g_registry.initialized || !bucket || !locations || !count) {
        return -1;
    }
    
    *locations = NULL;
    *count = 0;
    
    /* Get storage data directory */
    const buckets_storage_config_t *storage_config = buckets_storage_get_config();
    if (!storage_config || !storage_config->data_dir) {
        buckets_error("No storage data directory configured");
        return -1;
    }
    const char *base_dir = storage_config->data_dir;
    
    /* Allocate results array (will grow as needed) */
    size_t capacity = 64;
    size_t found = 0;
    buckets_object_location_t **results = buckets_calloc(capacity, sizeof(buckets_object_location_t*));
    if (!results) {
        return -1;
    }
    
    /* In multi-disk mode, scan disk1, disk2, etc. Otherwise scan base_dir directly */
    /* Try disk1 first to detect multi-disk mode */
    char data_dir[PATH_MAX];
    snprintf(data_dir, sizeof(data_dir), "%s/disk1", base_dir);
    
    DIR *test_dir = opendir(data_dir);
    if (!test_dir) {
        /* Not multi-disk mode, use base_dir directly */
        snprintf(data_dir, sizeof(data_dir), "%s", base_dir);
    } else {
        closedir(test_dir);
        /* Multi-disk mode - we'll scan disk1 only for now (registry entries are replicated) */
    }
    
    /* Scan all hash prefix directories (00-ff) */
    DIR *prefix_dir = opendir(data_dir);
    if (!prefix_dir) {
        buckets_free(results);
        return -1;
    }
    
    struct dirent *prefix_entry;
    while ((prefix_entry = readdir(prefix_dir)) != NULL) {
        /* Skip . and .. and hidden dirs except hash prefixes */
        if (prefix_entry->d_name[0] == '.') continue;
        
        /* Hash prefixes are 2 hex chars */
        if (strlen(prefix_entry->d_name) != 2) continue;
        
        /* Check if it's a hex prefix */
        char c1 = prefix_entry->d_name[0];
        char c2 = prefix_entry->d_name[1];
        bool is_hex = ((c1 >= '0' && c1 <= '9') || (c1 >= 'a' && c1 <= 'f')) &&
                      ((c2 >= '0' && c2 <= '9') || (c2 >= 'a' && c2 <= 'f'));
        if (!is_hex) continue;
        
        /* Open hash prefix directory */
        char prefix_path[PATH_MAX * 2];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(prefix_path, sizeof(prefix_path), "%s/%s", data_dir, prefix_entry->d_name);
#pragma GCC diagnostic pop
        
        DIR *hash_dir = opendir(prefix_path);
        if (!hash_dir) continue;
        
        struct dirent *hash_entry;
        while ((hash_entry = readdir(hash_dir)) != NULL) {
            if (hash_entry->d_name[0] == '.') continue;
            
            /* Check for max_keys limit */
            if (max_keys > 0 && found >= max_keys) {
                closedir(hash_dir);
                closedir(prefix_dir);
                goto done;
            }
            
            /* Read xl.meta */
            char meta_path[PATH_MAX * 2];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
            snprintf(meta_path, sizeof(meta_path), "%s/%s/xl.meta", 
                     prefix_path, hash_entry->d_name);
#pragma GCC diagnostic pop
            
            /* Read the file */
            FILE *f = fopen(meta_path, "r");
            if (!f) continue;
            
            fseek(f, 0, SEEK_END);
            long file_size = ftell(f);
            fseek(f, 0, SEEK_SET);
            
            if (file_size <= 0 || file_size > 1024 * 1024) {  /* Max 1MB */
                fclose(f);
                continue;
            }
            
            char *meta_content = buckets_malloc(file_size + 1);
            if (!meta_content) {
                fclose(f);
                continue;
            }
            
            size_t read_size = fread(meta_content, 1, file_size, f);
            fclose(f);
            meta_content[read_size] = '\0';
            
            /* Parse xl.meta JSON */
            cJSON *meta_json = cJSON_Parse(meta_content);
            buckets_free(meta_content);
            if (!meta_json) continue;
            
            /* Check if this is a registry entry (content-type: application/json) */
            cJSON *meta_obj = cJSON_GetObjectItem(meta_json, "meta");
            if (!meta_obj) {
                cJSON_Delete(meta_json);
                continue;
            }
            
            cJSON *content_type = cJSON_GetObjectItem(meta_obj, "content-type");
            if (!content_type || !cJSON_IsString(content_type) ||
                strcmp(content_type->valuestring, "application/json") != 0) {
                cJSON_Delete(meta_json);
                continue;
            }
            
            /* Get inline data (base64 encoded registry JSON) */
            cJSON *inline_data = cJSON_GetObjectItem(meta_json, "inline");
            if (!inline_data || !cJSON_IsString(inline_data)) {
                cJSON_Delete(meta_json);
                continue;
            }
            
            /* Decode and parse registry entry */
            size_t decoded_len = 0;
            char *decoded = decode_base64_inline(inline_data->valuestring, &decoded_len);
            cJSON_Delete(meta_json);
            
            if (!decoded) continue;
            
            buckets_object_location_t *loc = parse_registry_json(decoded);
            buckets_free(decoded);
            
            if (!loc || !loc->bucket) {
                if (loc) buckets_registry_location_free(loc);
                continue;
            }
            
            /* Check if this entry matches our bucket */
            if (strcmp(loc->bucket, bucket) != 0) {
                buckets_registry_location_free(loc);
                continue;
            }
            
            /* Check prefix filter if specified */
            if (prefix && loc->object) {
                if (strncmp(loc->object, prefix, strlen(prefix)) != 0) {
                    buckets_registry_location_free(loc);
                    continue;
                }
            }
            
            /* Skip delete markers (version_id starts with "delete-") */
            if (loc->version_id && strncmp(loc->version_id, "delete-", 7) == 0) {
                buckets_registry_location_free(loc);
                continue;
            }
            
            /* Add to results (grow array if needed) */
            if (found >= capacity) {
                capacity *= 2;
                buckets_object_location_t **new_results = buckets_realloc(
                    results, capacity * sizeof(buckets_object_location_t*));
                if (!new_results) {
                    buckets_registry_location_free(loc);
                    continue;
                }
                results = new_results;
            }
            
            results[found++] = loc;
        }
        
        closedir(hash_dir);
    }
    
    closedir(prefix_dir);
    
done:
    *locations = results;
    *count = found;
    
    buckets_debug("Registry list: bucket=%s, prefix=%s, found=%zu", 
                  bucket, prefix ? prefix : "(none)", found);
    
    return 0;
}
