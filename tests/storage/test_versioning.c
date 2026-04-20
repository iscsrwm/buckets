/**
 * Versioning Test Suite
 * 
 * Tests for S3-compatible object versioning functionality:
 * - Version ID generation
 * - Versioned object PUT/GET
 * - Delete markers
 * - Version listing
 * - Hard delete of versions
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_storage.h"

static char test_dir[256];

static void setup(void)
{
    /* Create unique test directory */
    snprintf(test_dir, sizeof(test_dir), "/tmp/buckets_version_test_%d", getpid());
    mkdir(test_dir, 0755);
    
    buckets_init();
    
    buckets_storage_config_t config = {
        .data_dir = test_dir,
        .inline_threshold = 128 * 1024,
        .default_ec_k = 0,  /* Use inline storage for tests */
        .default_ec_m = 0,
        .verify_checksums = true
    };
    
    buckets_storage_init(&config);
    buckets_metadata_cache_init(100, 60);
}

static void teardown(void)
{
    buckets_metadata_cache_cleanup();
    buckets_storage_cleanup();
    buckets_cleanup();
    
    /* Clean up test directory */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    int result = system(cmd);
    (void)result;
}

/* ===== Version ID Generation Tests ===== */

Test(versioning, generate_version_id, .init = setup, .fini = teardown)
{
    char version_id[37];
    int ret = buckets_generate_version_id(version_id);
    
    cr_assert_eq(ret, 0, "Version ID generation should succeed");
    cr_assert_eq(strlen(version_id), 36, "Version ID should be 36 chars (UUID format)");
    
    /* Check UUID format: 8-4-4-4-12 */
    cr_assert_eq(version_id[8], '-', "UUID format check");
    cr_assert_eq(version_id[13], '-', "UUID format check");
    cr_assert_eq(version_id[18], '-', "UUID format check");
    cr_assert_eq(version_id[23], '-', "UUID format check");
}

Test(versioning, unique_version_ids, .init = setup, .fini = teardown)
{
    char id1[37], id2[37], id3[37];
    
    buckets_generate_version_id(id1);
    buckets_generate_version_id(id2);
    buckets_generate_version_id(id3);
    
    cr_assert_str_neq(id1, id2, "Version IDs should be unique");
    cr_assert_str_neq(id2, id3, "Version IDs should be unique");
    cr_assert_str_neq(id1, id3, "Version IDs should be unique");
}

/* ===== ETag Tests ===== */

Test(versioning, etag_computation, .init = setup, .fini = teardown)
{
    const char *data = "Hello, World!";
    char etag[65];
    
    int ret = buckets_compute_etag(data, strlen(data), etag);
    
    cr_assert_eq(ret, 0, "ETag computation should succeed");
    cr_assert_eq(strlen(etag), 64, "ETag should be 64 hex chars (BLAKE2b-256)");
}

Test(versioning, etag_deterministic, .init = setup, .fini = teardown)
{
    const char *data = "Test data for determinism check";
    char etag1[65], etag2[65];
    
    buckets_compute_etag(data, strlen(data), etag1);
    buckets_compute_etag(data, strlen(data), etag2);
    
    cr_assert_str_eq(etag1, etag2, "Same data should produce same ETag");
}

Test(versioning, etag_different_data, .init = setup, .fini = teardown)
{
    char etag1[65], etag2[65];
    
    buckets_compute_etag("data1", 5, etag1);
    buckets_compute_etag("data2", 5, etag2);
    
    cr_assert_str_neq(etag1, etag2, "Different data should produce different ETags");
}

/* ===== User Metadata Tests ===== */

Test(versioning, user_metadata_add_get, .init = setup, .fini = teardown)
{
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    
    int ret = buckets_add_user_metadata(&meta, "author", "John Doe");
    cr_assert_eq(ret, 0, "Adding user metadata should succeed");
    
    const char *value = buckets_get_user_metadata(&meta, "author");
    cr_assert_not_null(value, "Getting user metadata should succeed");
    cr_assert_str_eq(value, "John Doe", "Retrieved value should match");
    
    buckets_xl_meta_free(&meta);
}

Test(versioning, user_metadata_multiple, .init = setup, .fini = teardown)
{
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    
    buckets_add_user_metadata(&meta, "key1", "value1");
    buckets_add_user_metadata(&meta, "key2", "value2");
    buckets_add_user_metadata(&meta, "key3", "value3");
    
    cr_assert_str_eq(buckets_get_user_metadata(&meta, "key1"), "value1");
    cr_assert_str_eq(buckets_get_user_metadata(&meta, "key2"), "value2");
    cr_assert_str_eq(buckets_get_user_metadata(&meta, "key3"), "value3");
    
    buckets_xl_meta_free(&meta);
}

Test(versioning, user_metadata_not_found, .init = setup, .fini = teardown)
{
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    
    const char *value = buckets_get_user_metadata(&meta, "nonexistent");
    cr_assert_null(value, "Non-existent key should return NULL");
    
    buckets_xl_meta_free(&meta);
}

/* ===== Metadata Cache Tests ===== */

Test(versioning, cache_put_get, .init = setup, .fini = teardown)
{
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    meta.stat.size = 1024;
    strcpy(meta.stat.modTime, "2026-03-04T00:00:00Z");
    
    int ret = buckets_metadata_cache_put("test-bucket", "test-object", NULL, &meta);
    cr_assert_eq(ret, 0, "Cache put should succeed");
    
    buckets_xl_meta_t retrieved = {0};
    ret = buckets_metadata_cache_get("test-bucket", "test-object", NULL, &retrieved);
    cr_assert_eq(ret, 0, "Cache get should succeed");
    cr_assert_eq(retrieved.stat.size, 1024, "Size should match");
    
    buckets_xl_meta_free(&retrieved);
}

Test(versioning, cache_invalidate, .init = setup, .fini = teardown)
{
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    meta.stat.size = 2048;
    
    buckets_metadata_cache_put("bucket", "object", NULL, &meta);
    buckets_metadata_cache_invalidate("bucket", "object", NULL);
    
    buckets_xl_meta_t retrieved = {0};
    int ret = buckets_metadata_cache_get("bucket", "object", NULL, &retrieved);
    cr_assert_neq(ret, 0, "Cache get after invalidate should fail");
}

Test(versioning, cache_stats, .init = setup, .fini = teardown)
{
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    
    /* Cause a miss */
    buckets_xl_meta_t temp = {0};
    buckets_metadata_cache_get("nonexistent", "key", NULL, &temp);
    
    /* Put and hit */
    buckets_metadata_cache_put("bucket", "key", NULL, &meta);
    buckets_metadata_cache_get("bucket", "key", NULL, &temp);
    buckets_xl_meta_free(&temp);
    
    u64 hits, misses, evictions;
    u32 count;
    buckets_metadata_cache_stats(&hits, &misses, &evictions, &count);
    
    cr_assert_geq(hits, 1ULL, "Should have at least 1 hit");
    cr_assert_geq(misses, 1ULL, "Should have at least 1 miss");
}

/* ===== List Versions Tests ===== */

Test(versioning, list_versions_empty, .init = setup, .fini = teardown)
{
    char **versions = NULL;
    bool *delete_markers = NULL;
    u32 count = 0;
    
    int ret = buckets_list_versions("test-bucket", "nonexistent-object",
                                     &versions, &delete_markers, &count);
    
    cr_assert_eq(ret, 0, "List versions of non-existent object should succeed");
    cr_assert_eq(count, 0, "Count should be 0");
    cr_assert_null(versions, "Versions array should be NULL");
    cr_assert_null(delete_markers, "Delete markers array should be NULL");
}

/* ===== NULL Input Handling ===== */

Test(versioning, null_inputs_version_id, .init = setup, .fini = teardown)
{
    int ret = buckets_generate_version_id(NULL);
    cr_assert_neq(ret, 0, "NULL version_id should fail");
}

Test(versioning, null_inputs_etag, .init = setup, .fini = teardown)
{
    char etag[65];
    
    int ret = buckets_compute_etag(NULL, 0, etag);
    cr_assert_neq(ret, 0, "NULL data should fail");
    
    ret = buckets_compute_etag("data", 4, NULL);
    cr_assert_neq(ret, 0, "NULL etag should fail");
}

Test(versioning, null_inputs_metadata, .init = setup, .fini = teardown)
{
    buckets_xl_meta_t meta = {0};
    
    int ret = buckets_add_user_metadata(NULL, "key", "value");
    cr_assert_neq(ret, 0, "NULL meta should fail");
    
    ret = buckets_add_user_metadata(&meta, NULL, "value");
    cr_assert_neq(ret, 0, "NULL key should fail");
    
    ret = buckets_add_user_metadata(&meta, "key", NULL);
    cr_assert_neq(ret, 0, "NULL value should fail");
    
    const char *val = buckets_get_user_metadata(NULL, "key");
    cr_assert_null(val, "NULL meta should return NULL");
    
    val = buckets_get_user_metadata(&meta, NULL);
    cr_assert_null(val, "NULL key should return NULL");
}

Test(versioning, null_inputs_cache, .init = setup, .fini = teardown)
{
    buckets_xl_meta_t meta = {0};
    
    int ret = buckets_metadata_cache_put(NULL, "object", NULL, &meta);
    cr_assert_neq(ret, 0, "NULL bucket should fail");
    
    ret = buckets_metadata_cache_put("bucket", NULL, NULL, &meta);
    cr_assert_neq(ret, 0, "NULL object should fail");
    
    ret = buckets_metadata_cache_put("bucket", "object", NULL, NULL);
    cr_assert_neq(ret, 0, "NULL meta should fail");
}

Test(versioning, null_inputs_list_versions, .init = setup, .fini = teardown)
{
    char **versions;
    bool *markers;
    u32 count;
    
    int ret = buckets_list_versions(NULL, "object", &versions, &markers, &count);
    cr_assert_neq(ret, 0, "NULL bucket should fail");
    
    ret = buckets_list_versions("bucket", NULL, &versions, &markers, &count);
    cr_assert_neq(ret, 0, "NULL object should fail");
}
