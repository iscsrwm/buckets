/**
 * Registry Storage Integration Tests
 * 
 * Tests that registry correctly persists to and loads from storage layer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_registry.h"
#include "buckets_storage.h"

#define TEST_DATA_DIR "/tmp/buckets-registry-test"

static void cleanup_test_dir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DATA_DIR);
    int ret = system(cmd);
    (void)ret;  /* Ignore result */
}

static void setup_test_dir(void)
{
    cleanup_test_dir();
    mkdir(TEST_DATA_DIR, 0755);
}

static void test_persist_and_reload(void)
{
    printf("Test 1: Persist and reload from storage\n");
    
    setup_test_dir();
    
    /* Initialize */
    assert(buckets_init() == 0);
    
    buckets_storage_config_t storage_config = {
        .data_dir = TEST_DATA_DIR,
        .inline_threshold = 128 * 1024,
        .default_ec_k = 8,
        .default_ec_m = 4,
        .verify_checksums = true
    };
    assert(buckets_storage_init(&storage_config) == 0);
    assert(buckets_registry_init(NULL) == 0);
    
    /* Create and record a location */
    buckets_object_location_t loc = {0};
    loc.bucket = "test-bucket";
    loc.object = "test-object";
    loc.version_id = "v1";
    loc.pool_idx = 0;
    loc.set_idx = 2;
    loc.disk_count = 12;
    for (u32 i = 0; i < loc.disk_count; i++) {
        loc.disk_idxs[i] = i;
    }
    loc.generation = 1;
    loc.mod_time = 1234567890;
    loc.size = 1024000;
    
    /* Record (should write to storage) */
    int result = buckets_registry_record(&loc);
    assert(result == 0);
    printf("  ✓ Recorded location to storage\n");
    
    /* Clear cache to force storage read */
    buckets_registry_cache_clear();
    printf("  ✓ Cleared cache\n");
    
    /* Lookup (should read from storage) */
    buckets_object_location_t *retrieved = NULL;
    result = buckets_registry_lookup("test-bucket", "test-object", "v1", &retrieved);
    assert(result == 0);
    assert(retrieved != NULL);
    printf("  ✓ Retrieved location from storage\n");
    
    /* Verify data */
    assert(strcmp(retrieved->bucket, "test-bucket") == 0);
    assert(strcmp(retrieved->object, "test-object") == 0);
    assert(strcmp(retrieved->version_id, "v1") == 0);
    assert(retrieved->pool_idx == 0);
    assert(retrieved->set_idx == 2);
    assert(retrieved->disk_count == 12);
    assert(retrieved->generation == 1);
    assert(retrieved->mod_time == 1234567890);
    assert(retrieved->size == 1024000);
    printf("  ✓ Data matches original\n");
    
    buckets_registry_location_free(retrieved);
    buckets_registry_cleanup();
    buckets_storage_cleanup();
    buckets_cleanup();
    cleanup_test_dir();
    
    printf("✓ Persist and reload successful\n");
}

static void test_cache_miss_loads_from_storage(void)
{
    printf("\nTest 2: Cache miss loads from storage\n");
    
    setup_test_dir();
    
    assert(buckets_init() == 0);
    
    buckets_storage_config_t storage_config = {
        .data_dir = TEST_DATA_DIR,
        .inline_threshold = 128 * 1024,
        .default_ec_k = 8,
        .default_ec_m = 4,
        .verify_checksums = true
    };
    assert(buckets_storage_init(&storage_config) == 0);
    assert(buckets_registry_init(NULL) == 0);
    
    /* Record multiple locations */
    for (int i = 0; i < 10; i++) {
        buckets_object_location_t loc = {0};
        char bucket[64], object[64], version[64];
        snprintf(bucket, sizeof(bucket), "bucket-%d", i);
        snprintf(object, sizeof(object), "object-%d", i);
        snprintf(version, sizeof(version), "v-%d", i);
        
        loc.bucket = bucket;
        loc.object = object;
        loc.version_id = version;
        loc.pool_idx = i % 2;
        loc.set_idx = i % 4;
        loc.disk_count = 12;
        loc.generation = 1;
        loc.size = 1024 * (i + 1);
        
        int result = buckets_registry_record(&loc);
        assert(result == 0);
    }
    printf("  ✓ Recorded 10 locations\n");
    
    /* Get initial stats (should be all cache hits) */
    buckets_registry_stats_t stats_before;
    buckets_registry_get_stats(&stats_before);
    printf("  Stats before clear: hits=%lu, misses=%lu, entries=%lu\n",
           stats_before.hits, stats_before.misses, stats_before.total_entries);
    
    /* Clear cache */
    buckets_registry_cache_clear();
    
    /* Lookup all locations (should all be storage reads) */
    for (int i = 0; i < 10; i++) {
        char bucket[64], object[64], version[64];
        snprintf(bucket, sizeof(bucket), "bucket-%d", i);
        snprintf(object, sizeof(object), "object-%d", i);
        snprintf(version, sizeof(version), "v-%d", i);
        
        buckets_object_location_t *retrieved = NULL;
        int result = buckets_registry_lookup(bucket, object, version, &retrieved);
        assert(result == 0);
        assert(retrieved != NULL);
        assert(retrieved->pool_idx == (u32)(i % 2));
        assert(retrieved->set_idx == (u32)(i % 4));
        assert(retrieved->size == (size_t)(1024 * (i + 1)));
        
        buckets_registry_location_free(retrieved);
    }
    printf("  ✓ All 10 locations retrieved from storage\n");
    
    /* Get stats after (should show cache misses) */
    buckets_registry_stats_t stats_after;
    buckets_registry_get_stats(&stats_after);
    printf("  Stats after storage reads: hits=%lu, misses=%lu, entries=%lu\n",
           stats_after.hits, stats_after.misses, stats_after.total_entries);
    
    /* Stats should show 10 cache misses (which loaded from storage) */
    assert(stats_after.total_entries == 10);
    printf("  ✓ Cache repopulated with %lu entries\n", stats_after.total_entries);
    
    buckets_registry_cleanup();
    buckets_storage_cleanup();
    buckets_cleanup();
    cleanup_test_dir();
    
    printf("✓ Cache miss storage loading successful\n");
}

static void test_delete_from_storage(void)
{
    printf("\nTest 3: Delete from storage\n");
    
    setup_test_dir();
    
    assert(buckets_init() == 0);
    
    buckets_storage_config_t storage_config = {
        .data_dir = TEST_DATA_DIR,
        .inline_threshold = 128 * 1024,
        .default_ec_k = 8,
        .default_ec_m = 4,
        .verify_checksums = true
    };
    assert(buckets_storage_init(&storage_config) == 0);
    assert(buckets_registry_init(NULL) == 0);
    
    /* Record a location */
    buckets_object_location_t loc = {0};
    loc.bucket = "delete-bucket";
    loc.object = "delete-object";
    loc.version_id = "v1";
    loc.pool_idx = 0;
    loc.set_idx = 1;
    loc.disk_count = 12;
    loc.generation = 1;
    loc.size = 2048;
    
    int result = buckets_registry_record(&loc);
    assert(result == 0);
    printf("  ✓ Recorded location\n");
    
    /* Verify it exists */
    buckets_object_location_t *retrieved = NULL;
    result = buckets_registry_lookup("delete-bucket", "delete-object", "v1", &retrieved);
    assert(result == 0);
    assert(retrieved != NULL);
    buckets_registry_location_free(retrieved);
    printf("  ✓ Location exists\n");
    
    /* Delete */
    result = buckets_registry_delete("delete-bucket", "delete-object", "v1");
    assert(result == 0);
    printf("  ✓ Deleted location\n");
    
    /* Verify it's gone (clear cache first to force storage check) */
    buckets_registry_cache_clear();
    retrieved = NULL;
    result = buckets_registry_lookup("delete-bucket", "delete-object", "v1", &retrieved);
    assert(result == -1);  /* Should not be found */
    assert(retrieved == NULL);
    printf("  ✓ Location no longer exists\n");
    
    buckets_registry_cleanup();
    buckets_storage_cleanup();
    buckets_cleanup();
    cleanup_test_dir();
    
    printf("✓ Delete from storage successful\n");
}

int main(void)
{
    printf("=== Registry Storage Integration Tests ===\n\n");
    
    test_persist_and_reload();
    test_cache_miss_loads_from_storage();
    test_delete_from_storage();
    
    printf("\n=== All Tests Passed! ===\n");
    return 0;
}
