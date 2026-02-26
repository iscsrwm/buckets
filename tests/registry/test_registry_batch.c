/**
 * Batch Operations Tests for Location Registry
 * 
 * Tests batch record, batch lookup, and update operations.
 * Requires storage layer initialization.
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "buckets.h"
#include "buckets_registry.h"
#include "buckets_storage.h"

#define TEST_DATA_DIR "/tmp/buckets-batch-test"

/* Test fixtures */

static void cleanup_test_dir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DATA_DIR);
    int ret = system(cmd);
    (void)ret;
}

void setup_registry(void)
{
    cleanup_test_dir();
    mkdir(TEST_DATA_DIR, 0755);
    
    buckets_init();
    
    buckets_storage_config_t storage_config = {
        .data_dir = TEST_DATA_DIR,
        .inline_threshold = 128 * 1024,
        .default_ec_k = 8,
        .default_ec_m = 4,
        .verify_checksums = true
    };
    buckets_storage_init(&storage_config);
    buckets_registry_init(NULL);
}

void teardown_registry(void)
{
    buckets_registry_cleanup();
    buckets_storage_cleanup();
    cleanup_test_dir();
}

TestSuite(registry_batch, .init = setup_registry, .fini = teardown_registry);

/* ===== Batch Record Tests ===== */

Test(registry_batch, record_multiple_locations)
{
    /* Create 10 test locations */
    buckets_object_location_t locations[10];
    
    for (int i = 0; i < 10; i++) {
        char bucket[64], object[64], version[64];
        snprintf(bucket, sizeof(bucket), "test-bucket-%d", i);
        snprintf(object, sizeof(object), "test-object-%d", i);
        snprintf(version, sizeof(version), "version-%d", i);
        
        locations[i].bucket = strdup(bucket);
        locations[i].object = strdup(object);
        locations[i].version_id = strdup(version);
        locations[i].pool_idx = 0;
        locations[i].set_idx = i % 4;
        locations[i].disk_count = 12;
        locations[i].generation = 1;
        locations[i].mod_time = time(NULL);
        locations[i].size = 1024 * (i + 1);
        
        for (u32 j = 0; j < 12; j++) {
            locations[i].disk_idxs[j] = j;
        }
    }
    
    /* Record all locations in batch */
    int result = buckets_registry_record_batch(locations, 10);
    cr_assert_eq(result, 10, "Should successfully record all 10 locations");
    
    /* Verify each location can be looked up */
    for (int i = 0; i < 10; i++) {
        buckets_object_location_t *loc = NULL;
        int lookup_result = buckets_registry_lookup(locations[i].bucket,
                                                     locations[i].object,
                                                     locations[i].version_id,
                                                     &loc);
        cr_assert_eq(lookup_result, 0, "Lookup should succeed for location %d", i);
        cr_assert_not_null(loc, "Location should not be NULL");
        cr_assert_eq(loc->set_idx, i % 4, "Set index should match");
        cr_assert_eq(loc->size, 1024 * (i + 1), "Size should match");
        
        buckets_registry_location_free(loc);
    }
    
    /* Cleanup */
    for (int i = 0; i < 10; i++) {
        free(locations[i].bucket);
        free(locations[i].object);
        free(locations[i].version_id);
    }
}

Test(registry_batch, record_batch_partial_failure)
{
    /* Create test locations with one invalid entry */
    buckets_object_location_t locations[3];
    
    /* Valid location 1 */
    locations[0].bucket = strdup("bucket1");
    locations[0].object = strdup("object1");
    locations[0].version_id = strdup("version1");
    locations[0].pool_idx = 0;
    locations[0].set_idx = 0;
    locations[0].disk_count = 12;
    locations[0].generation = 1;
    locations[0].mod_time = time(NULL);
    locations[0].size = 1024;
    
    /* Invalid location (NULL bucket) */
    locations[1].bucket = NULL;
    locations[1].object = strdup("object2");
    locations[1].version_id = strdup("version2");
    locations[1].pool_idx = 0;
    locations[1].set_idx = 1;
    locations[1].disk_count = 12;
    locations[1].generation = 1;
    locations[1].mod_time = time(NULL);
    locations[1].size = 2048;
    
    /* Valid location 2 */
    locations[2].bucket = strdup("bucket3");
    locations[2].object = strdup("object3");
    locations[2].version_id = strdup("version3");
    locations[2].pool_idx = 0;
    locations[2].set_idx = 2;
    locations[2].disk_count = 12;
    locations[2].generation = 1;
    locations[2].mod_time = time(NULL);
    locations[2].size = 3072;
    
    /* Record batch - should succeed for 2 out of 3 */
    int result = buckets_registry_record_batch(locations, 3);
    cr_assert_eq(result, 2, "Should record 2 valid locations");
    
    /* Cleanup */
    free(locations[0].bucket);
    free(locations[0].object);
    free(locations[0].version_id);
    free(locations[1].object);
    free(locations[1].version_id);
    free(locations[2].bucket);
    free(locations[2].object);
    free(locations[2].version_id);
}

/* ===== Batch Lookup Tests ===== */

Test(registry_batch, lookup_multiple_locations)
{
    /* First, record some test locations */
    for (int i = 0; i < 5; i++) {
        buckets_object_location_t loc;
        char bucket[64], object[64], version[64];
        
        snprintf(bucket, sizeof(bucket), "batch-bucket-%d", i);
        snprintf(object, sizeof(object), "batch-object-%d", i);
        snprintf(version, sizeof(version), "batch-version-%d", i);
        
        loc.bucket = bucket;
        loc.object = object;
        loc.version_id = version;
        loc.pool_idx = 0;
        loc.set_idx = i;
        loc.disk_count = 12;
        loc.generation = 1;
        loc.mod_time = time(NULL);
        loc.size = 1024 * i;
        
        for (u32 j = 0; j < 12; j++) {
            loc.disk_idxs[j] = j;
        }
        
        buckets_registry_record(&loc);
    }
    
    /* Create lookup keys */
    buckets_registry_key_t keys[5];
    char *buckets_mem[5], *objects_mem[5], *versions_mem[5];
    
    for (int i = 0; i < 5; i++) {
        buckets_mem[i] = malloc(64);
        objects_mem[i] = malloc(64);
        versions_mem[i] = malloc(64);
        
        snprintf(buckets_mem[i], 64, "batch-bucket-%d", i);
        snprintf(objects_mem[i], 64, "batch-object-%d", i);
        snprintf(versions_mem[i], 64, "batch-version-%d", i);
        
        keys[i].bucket = buckets_mem[i];
        keys[i].object = objects_mem[i];
        keys[i].version_id = versions_mem[i];
    }
    
    /* Lookup batch */
    buckets_object_location_t **locations = NULL;
    int result = buckets_registry_lookup_batch(keys, 5, &locations);
    
    cr_assert_eq(result, 5, "Should find all 5 locations");
    cr_assert_not_null(locations, "Locations array should not be NULL");
    
    /* Verify each location */
    for (int i = 0; i < 5; i++) {
        cr_assert_not_null(locations[i], "Location %d should not be NULL", i);
        cr_assert_eq(locations[i]->set_idx, (u32)i, "Set index should match");
        cr_assert_eq(locations[i]->size, 1024 * (size_t)i, "Size should match");
        
        buckets_registry_location_free(locations[i]);
    }
    
    /* Cleanup */
    free(locations);
    for (int i = 0; i < 5; i++) {
        free(buckets_mem[i]);
        free(objects_mem[i]);
        free(versions_mem[i]);
    }
}

Test(registry_batch, lookup_batch_with_missing)
{
    /* Record 2 out of 3 locations */
    buckets_object_location_t loc;
    
    loc.bucket = "exist-bucket-1";
    loc.object = "exist-object-1";
    loc.version_id = "exist-version-1";
    loc.pool_idx = 0;
    loc.set_idx = 0;
    loc.disk_count = 12;
    loc.generation = 1;
    loc.mod_time = time(NULL);
    loc.size = 1024;
    buckets_registry_record(&loc);
    
    loc.bucket = "exist-bucket-2";
    loc.object = "exist-object-2";
    loc.version_id = "exist-version-2";
    loc.set_idx = 1;
    buckets_registry_record(&loc);
    
    /* Create lookup keys with one missing */
    buckets_registry_key_t keys[3];
    keys[0].bucket = "exist-bucket-1";
    keys[0].object = "exist-object-1";
    keys[0].version_id = "exist-version-1";
    
    keys[1].bucket = "missing-bucket";
    keys[1].object = "missing-object";
    keys[1].version_id = "missing-version";
    
    keys[2].bucket = "exist-bucket-2";
    keys[2].object = "exist-object-2";
    keys[2].version_id = "exist-version-2";
    
    /* Lookup batch */
    buckets_object_location_t **locations = NULL;
    int result = buckets_registry_lookup_batch(keys, 3, &locations);
    
    cr_assert_eq(result, 2, "Should find 2 out of 3 locations");
    cr_assert_not_null(locations, "Locations array should not be NULL");
    
    /* Verify results */
    cr_assert_not_null(locations[0], "First location should be found");
    cr_assert_null(locations[1], "Second location should be NULL (not found)");
    cr_assert_not_null(locations[2], "Third location should be found");
    
    /* Cleanup */
    buckets_registry_location_free(locations[0]);
    buckets_registry_location_free(locations[2]);
    free(locations);
}

/* ===== Update Operation Tests ===== */

Test(registry_batch, update_location)
{
    /* Record initial location */
    buckets_object_location_t loc;
    loc.bucket = "update-bucket";
    loc.object = "update-object";
    loc.version_id = "update-version";
    loc.pool_idx = 0;
    loc.set_idx = 0;
    loc.disk_count = 12;
    loc.generation = 1;
    loc.mod_time = time(NULL);
    loc.size = 1024;
    
    for (u32 i = 0; i < 12; i++) {
        loc.disk_idxs[i] = i;
    }
    
    int result = buckets_registry_record(&loc);
    cr_assert_eq(result, 0, "Initial record should succeed");
    
    /* Verify initial location */
    buckets_object_location_t *lookup_loc = NULL;
    buckets_registry_lookup("update-bucket", "update-object", "update-version", &lookup_loc);
    cr_assert_not_null(lookup_loc, "Initial lookup should succeed");
    cr_assert_eq(lookup_loc->set_idx, 0, "Initial set_idx should be 0");
    cr_assert_eq(lookup_loc->size, 1024, "Initial size should be 1024");
    buckets_registry_location_free(lookup_loc);
    
    /* Update location (simulate migration to different set) */
    buckets_object_location_t new_loc = loc;
    new_loc.set_idx = 3;
    new_loc.generation = 2;
    new_loc.size = 2048;
    
    result = buckets_registry_update("update-bucket", "update-object", 
                                     "update-version", &new_loc);
    cr_assert_eq(result, 0, "Update should succeed");
    
    /* Verify updated location */
    lookup_loc = NULL;
    buckets_registry_lookup("update-bucket", "update-object", "update-version", &lookup_loc);
    cr_assert_not_null(lookup_loc, "Updated lookup should succeed");
    cr_assert_eq(lookup_loc->set_idx, 3, "Updated set_idx should be 3");
    cr_assert_eq(lookup_loc->generation, 2, "Updated generation should be 2");
    cr_assert_eq(lookup_loc->size, 2048, "Updated size should be 2048");
    
    buckets_registry_location_free(lookup_loc);
}

Test(registry_batch, update_nonexistent_location)
{
    /* Try to update a location that doesn't exist */
    buckets_object_location_t loc;
    loc.bucket = "nonexist-bucket";
    loc.object = "nonexist-object";
    loc.version_id = "nonexist-version";
    loc.pool_idx = 0;
    loc.set_idx = 0;
    loc.disk_count = 12;
    loc.generation = 1;
    loc.mod_time = time(NULL);
    loc.size = 1024;
    
    /* Update should succeed (creates new entry like record) */
    int result = buckets_registry_update("nonexist-bucket", "nonexist-object",
                                         "nonexist-version", &loc);
    cr_assert_eq(result, 0, "Update of nonexistent location should succeed");
    
    /* Verify location was created */
    buckets_object_location_t *lookup_loc = NULL;
    buckets_registry_lookup("nonexist-bucket", "nonexist-object", 
                           "nonexist-version", &lookup_loc);
    cr_assert_not_null(lookup_loc, "Location should be created by update");
    
    buckets_registry_location_free(lookup_loc);
}
