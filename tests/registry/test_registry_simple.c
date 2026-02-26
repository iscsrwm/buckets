/**
 * Simple Registry Tests
 * 
 * Basic tests for location registry functionality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "buckets.h"
#include "buckets_registry.h"

static void test_init_cleanup(void)
{
    printf("Test 1: Init and cleanup\n");
    
    assert(buckets_init() == 0);
    assert(buckets_registry_init(NULL) == 0);
    
    const buckets_registry_config_t *config = buckets_registry_get_config();
    assert(config != NULL);
    assert(config->cache_size == BUCKETS_REGISTRY_CACHE_SIZE);
    assert(config->cache_ttl_seconds == BUCKETS_REGISTRY_CACHE_TTL);
    assert(config->enable_cache == true);
    
    buckets_registry_cleanup();
    buckets_cleanup();
    
    printf("✓ Init and cleanup successful\n");
}

static void test_location_serialization(void)
{
    printf("\nTest 2: Location serialization\n");
    
    assert(buckets_init() == 0);
    
    /* Create a location */
    buckets_object_location_t *loc = buckets_registry_location_new();
    assert(loc != NULL);
    
    loc->bucket = buckets_strdup("test-bucket");
    loc->object = buckets_strdup("test-object");
    loc->version_id = buckets_strdup("test-version-123");
    loc->pool_idx = 0;
    loc->set_idx = 2;
    loc->disk_count = 12;
    for (u32 i = 0; i < loc->disk_count; i++) {
        loc->disk_idxs[i] = i;
    }
    loc->generation = 1;
    loc->mod_time = 1234567890;
    loc->size = 1024000;
    
    /* Serialize to JSON */
    char *json = buckets_registry_location_to_json(loc);
    assert(json != NULL);
    printf("  JSON: %s\n", json);
    
    /* Deserialize from JSON */
    buckets_object_location_t *loc2 = buckets_registry_location_from_json(json);
    assert(loc2 != NULL);
    assert(strcmp(loc2->bucket, "test-bucket") == 0);
    assert(strcmp(loc2->object, "test-object") == 0);
    assert(strcmp(loc2->version_id, "test-version-123") == 0);
    assert(loc2->pool_idx == 0);
    assert(loc2->set_idx == 2);
    assert(loc2->disk_count == 12);
    assert(loc2->generation == 1);
    assert(loc2->mod_time == 1234567890);
    assert(loc2->size == 1024000);
    
    buckets_free(json);
    buckets_registry_location_free(loc);
    buckets_registry_location_free(loc2);
    buckets_cleanup();
    
    printf("✓ Serialization roundtrip successful\n");
}

static void test_location_clone(void)
{
    printf("\nTest 3: Location cloning\n");
    
    assert(buckets_init() == 0);
    
    buckets_object_location_t *loc = buckets_registry_location_new();
    loc->bucket = buckets_strdup("bucket1");
    loc->object = buckets_strdup("object1");
    loc->version_id = buckets_strdup("v1");
    loc->pool_idx = 1;
    loc->set_idx = 3;
    loc->disk_count = 8;
    loc->generation = 5;
    loc->size = 2048;
    
    /* Clone */
    buckets_object_location_t *clone = buckets_registry_location_clone(loc);
    assert(clone != NULL);
    assert(clone != loc);  /* Different pointers */
    assert(strcmp(clone->bucket, loc->bucket) == 0);
    assert(strcmp(clone->object, loc->object) == 0);
    assert(strcmp(clone->version_id, loc->version_id) == 0);
    assert(clone->pool_idx == loc->pool_idx);
    assert(clone->set_idx == loc->set_idx);
    assert(clone->disk_count == loc->disk_count);
    assert(clone->generation == loc->generation);
    assert(clone->size == loc->size);
    
    buckets_registry_location_free(loc);
    buckets_registry_location_free(clone);
    buckets_cleanup();
    
    printf("✓ Cloning successful\n");
}

static void test_registry_key_utils(void)
{
    printf("\nTest 4: Registry key utilities\n");
    
    assert(buckets_init() == 0);
    
    /* Build key */
    char *key = buckets_registry_build_key("my-bucket", "my-object", "version-123");
    assert(key != NULL);
    printf("  Key: %s\n", key);
    assert(strcmp(key, "my-bucket/my-object/version-123") == 0);
    
    /* Parse key */
    char *bucket = NULL, *object = NULL, *version_id = NULL;
    int result = buckets_registry_parse_key(key, &bucket, &object, &version_id);
    assert(result == 0);
    assert(strcmp(bucket, "my-bucket") == 0);
    assert(strcmp(object, "my-object") == 0);
    assert(strcmp(version_id, "version-123") == 0);
    
    buckets_free(key);
    buckets_free(bucket);
    buckets_free(object);
    buckets_free(version_id);
    buckets_cleanup();
    
    printf("✓ Key utilities successful\n");
}

static void test_cache_operations(void)
{
    printf("\nTest 5: Cache operations\n");
    
    assert(buckets_init() == 0);
    assert(buckets_registry_init(NULL) == 0);
    
    /* Create a location */
    buckets_object_location_t loc = {0};
    loc.bucket = "test-bucket";
    loc.object = "test-object";
    loc.version_id = "v1";
    loc.pool_idx = 0;
    loc.set_idx = 1;
    loc.disk_count = 12;
    loc.generation = 1;
    loc.size = 1024;
    
    /* Record (should update cache) */
    int result = buckets_registry_record(&loc);
    assert(result == 0);
    
    /* Lookup (should be cache hit) */
    buckets_object_location_t *retrieved = NULL;
    result = buckets_registry_lookup("test-bucket", "test-object", "v1", &retrieved);
    assert(result == 0);
    assert(retrieved != NULL);
    assert(strcmp(retrieved->bucket, "test-bucket") == 0);
    assert(strcmp(retrieved->object, "test-object") == 0);
    assert(strcmp(retrieved->version_id, "v1") == 0);
    assert(retrieved->pool_idx == 0);
    assert(retrieved->set_idx == 1);
    
    /* Get stats */
    buckets_registry_stats_t stats;
    result = buckets_registry_get_stats(&stats);
    assert(result == 0);
    printf("  Cache stats: hits=%lu, misses=%lu, entries=%lu, hit_rate=%.1f%%\n",
           stats.hits, stats.misses, stats.total_entries, stats.hit_rate);
    assert(stats.hits == 1);  /* One cache hit from lookup */
    assert(stats.total_entries == 1);
    
    /* Invalidate */
    result = buckets_registry_cache_invalidate("test-bucket", "test-object", "v1");
    assert(result == 0);
    
    /* Lookup again (should miss) */
    buckets_registry_location_free(retrieved);
    retrieved = NULL;
    result = buckets_registry_lookup("test-bucket", "test-object", "v1", &retrieved);
    assert(result == -1);  /* Not found after invalidation */
    
    buckets_registry_cleanup();
    buckets_cleanup();
    
    printf("✓ Cache operations successful\n");
}

int main(void)
{
    printf("=== Registry Simple Tests ===\n\n");
    
    test_init_cleanup();
    test_location_serialization();
    test_location_clone();
    test_registry_key_utils();
    test_cache_operations();
    
    printf("\n=== All Tests Passed! ===\n");
    return 0;
}
