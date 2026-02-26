/**
 * Registry Integration Tests
 * 
 * Tests that registry automatically tracks object PUT/GET/DELETE operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_registry.h"
#include "buckets_storage.h"

#define TEST_DATA_DIR "/tmp/buckets-registry-integration-test"

static void cleanup_test_dir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DATA_DIR);
    int ret = system(cmd);
    (void)ret;
}

static void setup_test_dir(void)
{
    cleanup_test_dir();
    mkdir(TEST_DATA_DIR, 0755);
}

/* Test 1: PUT object automatically records location */
static void test_put_records_location(void)
{
    printf("Test 1: PUT automatically records location in registry\n");
    
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
    
    /* PUT an object */
    const char *data = "Hello, Registry Integration!";
    int result = buckets_put_object("test-bucket", "test-object", 
                                     data, strlen(data), "text/plain");
    assert(result == 0);
    printf("  ✓ Object written successfully\n");
    
    /* Verify location was recorded in registry */
    buckets_object_location_t *loc = NULL;
    result = buckets_registry_lookup("test-bucket", "test-object", "latest", &loc);
    assert(result == 0);
    assert(loc != NULL);
    printf("  ✓ Location found in registry\n");
    
    /* Verify location details */
    assert(strcmp(loc->bucket, "test-bucket") == 0);
    assert(strcmp(loc->object, "test-object") == 0);
    assert(strcmp(loc->version_id, "latest") == 0);
    assert(loc->size == strlen(data));
    printf("  ✓ Location details correct (size=%zu)\n", loc->size);
    
    buckets_registry_location_free(loc);
    
    /* Cleanup */
    buckets_registry_cleanup();
    buckets_storage_cleanup();
    cleanup_test_dir();
    
    printf("  ✅ PASS\n\n");
}

/* Test 2: DELETE object removes location */
static void test_delete_removes_location(void)
{
    printf("Test 2: DELETE automatically removes location from registry\n");
    
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
    
    /* PUT an object */
    const char *data = "Test data for deletion";
    int result = buckets_put_object("del-bucket", "del-object",
                                     data, strlen(data), "text/plain");
    assert(result == 0);
    printf("  ✓ Object written\n");
    
    /* Verify it's in registry */
    buckets_object_location_t *loc = NULL;
    result = buckets_registry_lookup("del-bucket", "del-object", "latest", &loc);
    assert(result == 0);
    assert(loc != NULL);
    buckets_registry_location_free(loc);
    printf("  ✓ Location in registry before delete\n");
    
    /* DELETE the object */
    result = buckets_delete_object("del-bucket", "del-object");
    assert(result == 0);
    printf("  ✓ Object deleted\n");
    
    /* Verify it's removed from registry */
    loc = NULL;
    result = buckets_registry_lookup("del-bucket", "del-object", "latest", &loc);
    assert(result != 0);  /* Should not be found */
    assert(loc == NULL);
    printf("  ✓ Location removed from registry\n");
    
    /* Cleanup */
    buckets_registry_cleanup();
    buckets_storage_cleanup();
    cleanup_test_dir();
    
    printf("  ✅ PASS\n\n");
}

/* Test 3: Multiple objects tracked independently */
static void test_multiple_objects_tracked(void)
{
    printf("Test 3: Multiple objects tracked independently\n");
    
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
    
    /* PUT multiple objects */
    for (int i = 0; i < 5; i++) {
        char bucket[64], object[64], data[256];
        snprintf(bucket, sizeof(bucket), "bucket-%d", i);
        snprintf(object, sizeof(object), "object-%d", i);
        snprintf(data, sizeof(data), "Data for object %d", i);
        
        int result = buckets_put_object(bucket, object, data, strlen(data), "text/plain");
        assert(result == 0);
    }
    printf("  ✓ Written 5 objects\n");
    
    /* Verify all are in registry */
    for (int i = 0; i < 5; i++) {
        char bucket[64], object[64];
        snprintf(bucket, sizeof(bucket), "bucket-%d", i);
        snprintf(object, sizeof(object), "object-%d", i);
        
        buckets_object_location_t *loc = NULL;
        int result = buckets_registry_lookup(bucket, object, "latest", &loc);
        assert(result == 0);
        assert(loc != NULL);
        buckets_registry_location_free(loc);
    }
    printf("  ✓ All 5 objects found in registry\n");
    
    /* Delete one object */
    int result = buckets_delete_object("bucket-2", "object-2");
    assert(result == 0);
    printf("  ✓ Deleted one object\n");
    
    /* Verify it's gone but others remain */
    buckets_object_location_t *loc = NULL;
    result = buckets_registry_lookup("bucket-2", "object-2", "latest", &loc);
    assert(result != 0);  /* Should not be found */
    printf("  ✓ Deleted object not in registry\n");
    
    loc = NULL;
    result = buckets_registry_lookup("bucket-0", "object-0", "latest", &loc);
    assert(result == 0);  /* Others should still be there */
    assert(loc != NULL);
    buckets_registry_location_free(loc);
    printf("  ✓ Other objects still in registry\n");
    
    /* Cleanup */
    buckets_registry_cleanup();
    buckets_storage_cleanup();
    cleanup_test_dir();
    
    printf("  ✅ PASS\n\n");
}

/* Test 4: Cache hit after PUT */
static void test_cache_hit_after_put(void)
{
    printf("Test 4: Registry cache works after PUT\n");
    
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
    
    /* PUT an object */
    const char *data = "Cache test data";
    int result = buckets_put_object("cache-bucket", "cache-object",
                                     data, strlen(data), "text/plain");
    assert(result == 0);
    printf("  ✓ Object written\n");
    
    /* Get stats before lookup */
    buckets_registry_stats_t stats_before;
    buckets_registry_get_stats(&stats_before);
    u64 hits_before = stats_before.hits;
    
    /* Lookup (should be cache hit) */
    buckets_object_location_t *loc = NULL;
    result = buckets_registry_lookup("cache-bucket", "cache-object", "latest", &loc);
    assert(result == 0);
    assert(loc != NULL);
    buckets_registry_location_free(loc);
    
    /* Get stats after lookup */
    buckets_registry_stats_t stats_after;
    buckets_registry_get_stats(&stats_after);
    u64 hits_after = stats_after.hits;
    
    /* Verify it was a cache hit */
    assert(hits_after > hits_before);
    printf("  ✓ Cache hit detected (hits: %lu -> %lu)\n", hits_before, hits_after);
    
    /* Cleanup */
    buckets_registry_cleanup();
    buckets_storage_cleanup();
    cleanup_test_dir();
    
    printf("  ✅ PASS\n\n");
}

int main(void)
{
    printf("=========================================\n");
    printf("Registry Integration Tests\n");
    printf("=========================================\n\n");
    
    test_put_records_location();
    test_delete_removes_location();
    test_multiple_objects_tracked();
    test_cache_hit_after_put();
    
    printf("=========================================\n");
    printf("All Tests Passed!\n");
    printf("=========================================\n");
    
    return 0;
}
