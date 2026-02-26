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
    
    /* Delete from storage */
    char object_key[1024];
    snprintf(object_key, sizeof(object_key), "%s/%s/%s.json", bucket, object, vid);
    
    int result = buckets_delete_object(BUCKETS_REGISTRY_BUCKET, object_key);
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
