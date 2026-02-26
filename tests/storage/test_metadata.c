/**
 * Criterion Unit Tests for Metadata and Versioning
 * 
 * Comprehensive test suite for:
 * - ETag computation
 * - User metadata (x-amz-meta-*)
 * - S3 standard metadata fields
 * - Object versioning
 * - xl.meta serialization with new fields
 */

#include <criterion/criterion.h>
#include <criterion/redirect.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ctype.h>

#include "buckets.h"
#include "buckets_storage.h"

/* Test fixtures */
static char test_data_dir[PATH_MAX];

void setup(void) {
    buckets_log_init();
    buckets_set_log_level(BUCKETS_LOG_ERROR);  /* Quiet during tests */
    
    /* Create temporary test directory */
    snprintf(test_data_dir, sizeof(test_data_dir), "/tmp/buckets_meta_test_%d", getpid());
    mkdir(test_data_dir, 0755);
    
    /* Initialize storage */
    buckets_storage_config_t config = {
        .data_dir = test_data_dir,
        .inline_threshold = 128 * 1024,
        .default_ec_k = 8,
        .default_ec_m = 4,
        .verify_checksums = true
    };
    
    int result = buckets_storage_init(&config);
    cr_assert_eq(result, 0, "Storage init should succeed");
}

void teardown(void) {
    buckets_storage_cleanup();
    
    /* Clean up test directory */
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", test_data_dir);
    int ret = system(cmd);
    (void)ret;
}

/* ===== ETag Computation Tests ===== */

Test(metadata, compute_etag_basic, .init = setup, .fini = teardown) {
    const char *data = "Hello, World!";
    size_t size = strlen(data);
    char etag[65];
    
    int result = buckets_compute_etag(data, size, etag);
    cr_assert_eq(result, 0, "ETag computation should succeed");
    cr_assert_eq(strlen(etag), 64, "ETag should be 64 hex characters");
    
    /* Verify it's all hex */
    for (int i = 0; i < 64; i++) {
        cr_assert(isxdigit(etag[i]), "ETag should contain only hex digits");
    }
}

Test(metadata, compute_etag_deterministic, .init = setup, .fini = teardown) {
    const char *data = "Test data for deterministic hash";
    size_t size = strlen(data);
    char etag1[65], etag2[65];
    
    /* Compute twice */
    buckets_compute_etag(data, size, etag1);
    buckets_compute_etag(data, size, etag2);
    
    cr_assert_str_eq(etag1, etag2, "ETag should be deterministic");
}

Test(metadata, compute_etag_different_data, .init = setup, .fini = teardown) {
    const char *data1 = "Data one";
    const char *data2 = "Data two";
    char etag1[65], etag2[65];
    
    buckets_compute_etag(data1, strlen(data1), etag1);
    buckets_compute_etag(data2, strlen(data2), etag2);
    
    cr_assert_str_neq(etag1, etag2, "Different data should produce different ETags");
}

Test(metadata, compute_etag_large_data, .init = setup, .fini = teardown) {
    /* 1MB data */
    size_t size = 1024 * 1024;
    u8 *data = buckets_malloc(size);
    for (size_t i = 0; i < size; i++) {
        data[i] = (u8)(i % 256);
    }
    
    char etag[65];
    int result = buckets_compute_etag(data, size, etag);
    
    cr_assert_eq(result, 0, "ETag computation should succeed for large data");
    cr_assert_eq(strlen(etag), 64, "ETag length should be correct");
    
    buckets_free(data);
}

Test(metadata, compute_etag_null_params) {
    char etag[65];
    
    int result = buckets_compute_etag(NULL, 100, etag);
    cr_assert_eq(result, -1, "NULL data should fail");
    
    result = buckets_compute_etag("data", 4, NULL);
    cr_assert_eq(result, -1, "NULL etag buffer should fail");
}

/* ===== User Metadata Tests ===== */

Test(metadata, add_user_metadata, .init = setup, .fini = teardown) {
    buckets_xl_meta_t meta = {0};
    
    int result = buckets_add_user_metadata(&meta, "author", "John Doe");
    cr_assert_eq(result, 0, "Adding user metadata should succeed");
    cr_assert_eq(meta.meta.user_count, 1, "User count should be 1");
    cr_assert_str_eq(meta.meta.user_keys[0], "author", "Key should match");
    cr_assert_str_eq(meta.meta.user_values[0], "John Doe", "Value should match");
    
    buckets_xl_meta_free(&meta);
}

Test(metadata, add_multiple_user_metadata, .init = setup, .fini = teardown) {
    buckets_xl_meta_t meta = {0};
    
    buckets_add_user_metadata(&meta, "author", "John Doe");
    buckets_add_user_metadata(&meta, "project", "Buckets");
    buckets_add_user_metadata(&meta, "version", "1.0");
    
    cr_assert_eq(meta.meta.user_count, 3, "Should have 3 user metadata entries");
    
    const char *author = buckets_get_user_metadata(&meta, "author");
    cr_assert_str_eq(author, "John Doe", "Author should match");
    
    const char *project = buckets_get_user_metadata(&meta, "project");
    cr_assert_str_eq(project, "Buckets", "Project should match");
    
    const char *version = buckets_get_user_metadata(&meta, "version");
    cr_assert_str_eq(version, "1.0", "Version should match");
    
    buckets_xl_meta_free(&meta);
}

Test(metadata, update_user_metadata, .init = setup, .fini = teardown) {
    buckets_xl_meta_t meta = {0};
    
    buckets_add_user_metadata(&meta, "status", "draft");
    cr_assert_eq(meta.meta.user_count, 1, "Should have 1 entry");
    
    /* Update existing key */
    buckets_add_user_metadata(&meta, "status", "published");
    cr_assert_eq(meta.meta.user_count, 1, "Count should remain 1");
    
    const char *status = buckets_get_user_metadata(&meta, "status");
    cr_assert_str_eq(status, "published", "Value should be updated");
    
    buckets_xl_meta_free(&meta);
}

Test(metadata, get_nonexistent_user_metadata, .init = setup, .fini = teardown) {
    buckets_xl_meta_t meta = {0};
    
    buckets_add_user_metadata(&meta, "key1", "value1");
    
    const char *result = buckets_get_user_metadata(&meta, "nonexistent");
    cr_assert_null(result, "Nonexistent key should return NULL");
    
    buckets_xl_meta_free(&meta);
}

/* ===== Version ID Generation Tests ===== */

Test(metadata, generate_version_id, .init = setup, .fini = teardown) {
    char versionId[37];
    
    int result = buckets_generate_version_id(versionId);
    cr_assert_eq(result, 0, "Version ID generation should succeed");
    cr_assert_eq(strlen(versionId), 36, "Version ID should be 36 chars (UUID)");
    
    /* Check UUID format: 8-4-4-4-12 */
    cr_assert_eq(versionId[8], '-', "UUID dash at position 8");
    cr_assert_eq(versionId[13], '-', "UUID dash at position 13");
    cr_assert_eq(versionId[18], '-', "UUID dash at position 18");
    cr_assert_eq(versionId[23], '-', "UUID dash at position 23");
}

Test(metadata, generate_version_id_unique, .init = setup, .fini = teardown) {
    char versionId1[37], versionId2[37];
    
    buckets_generate_version_id(versionId1);
    buckets_generate_version_id(versionId2);
    
    cr_assert_str_neq(versionId1, versionId2, "Version IDs should be unique");
}

/* ===== Metadata Serialization Tests ===== */

Test(metadata, serialize_with_s3_metadata, .init = setup, .fini = teardown) {
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    meta.stat.size = 1024;
    strcpy(meta.stat.modTime, "2026-02-25T10:00:00Z");
    
    /* Add S3 metadata */
    meta.meta.content_type = buckets_strdup("image/jpeg");
    meta.meta.cache_control = buckets_strdup("max-age=3600");
    meta.meta.content_disposition = buckets_strdup("attachment; filename=photo.jpg");
    
    char *json = buckets_xl_meta_to_json(&meta);
    cr_assert_not_null(json, "JSON serialization should succeed");
    
    /* Verify fields are present */
    cr_assert(strstr(json, "content-type") != NULL, "Should contain content-type");
    cr_assert(strstr(json, "image/jpeg") != NULL, "Should contain content-type value");
    cr_assert(strstr(json, "cache-control") != NULL, "Should contain cache-control");
    cr_assert(strstr(json, "max-age=3600") != NULL, "Should contain cache-control value");
    cr_assert(strstr(json, "content-disposition") != NULL, "Should contain content-disposition");
    
    buckets_free(json);
    buckets_xl_meta_free(&meta);
}

Test(metadata, serialize_with_user_metadata, .init = setup, .fini = teardown) {
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    
    buckets_add_user_metadata(&meta, "author", "Alice");
    buckets_add_user_metadata(&meta, "project", "Test");
    
    char *json = buckets_xl_meta_to_json(&meta);
    cr_assert_not_null(json, "JSON serialization should succeed");
    
    /* User metadata should be prefixed */
    cr_assert(strstr(json, "x-amz-meta-author") != NULL, "Should have prefixed key");
    cr_assert(strstr(json, "Alice") != NULL, "Should have user value");
    cr_assert(strstr(json, "x-amz-meta-project") != NULL, "Should have prefixed key");
    cr_assert(strstr(json, "Test") != NULL, "Should have user value");
    
    buckets_free(json);
    buckets_xl_meta_free(&meta);
}

Test(metadata, serialize_with_versioning, .init = setup, .fini = teardown) {
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    
    meta.versioning.versionId = buckets_strdup("550e8400-e29b-41d4-a716-446655440000");
    meta.versioning.isLatest = true;
    meta.versioning.isDeleteMarker = false;
    
    char *json = buckets_xl_meta_to_json(&meta);
    cr_assert_not_null(json, "JSON serialization should succeed");
    
    cr_assert(strstr(json, "versioning") != NULL, "Should have versioning section");
    cr_assert(strstr(json, "versionId") != NULL, "Should have versionId");
    cr_assert(strstr(json, "550e8400-e29b-41d4-a716-446655440000") != NULL, 
             "Should have version ID value");
    cr_assert(strstr(json, "isLatest") != NULL, "Should have isLatest");
    
    buckets_free(json);
    buckets_xl_meta_free(&meta);
}

Test(metadata, deserialize_with_s3_metadata, .init = setup, .fini = teardown) {
    const char *json = "{"
        "\"version\": 1,"
        "\"format\": \"xl\","
        "\"stat\": {\"size\": 1024, \"modTime\": \"2026-02-25T10:00:00Z\"},"
        "\"meta\": {"
        "  \"content-type\": \"application/json\","
        "  \"cache-control\": \"no-cache\","
        "  \"content-encoding\": \"gzip\""
        "}"
    "}";
    
    buckets_xl_meta_t meta;
    int result = buckets_xl_meta_from_json(json, &meta);
    
    cr_assert_eq(result, 0, "Deserialization should succeed");
    cr_assert_str_eq(meta.meta.content_type, "application/json", "Content type should match");
    cr_assert_str_eq(meta.meta.cache_control, "no-cache", "Cache control should match");
    cr_assert_str_eq(meta.meta.content_encoding, "gzip", "Content encoding should match");
    
    buckets_xl_meta_free(&meta);
}

Test(metadata, deserialize_with_user_metadata, .init = setup, .fini = teardown) {
    const char *json = "{"
        "\"version\": 1,"
        "\"format\": \"xl\","
        "\"stat\": {\"size\": 100, \"modTime\": \"2026-02-25T10:00:00Z\"},"
        "\"meta\": {"
        "  \"x-amz-meta-author\": \"Bob\","
        "  \"x-amz-meta-department\": \"Engineering\""
        "}"
    "}";
    
    buckets_xl_meta_t meta;
    int result = buckets_xl_meta_from_json(json, &meta);
    
    cr_assert_eq(result, 0, "Deserialization should succeed");
    cr_assert_eq(meta.meta.user_count, 2, "Should have 2 user metadata entries");
    
    const char *author = buckets_get_user_metadata(&meta, "author");
    cr_assert_str_eq(author, "Bob", "Author should match");
    
    const char *dept = buckets_get_user_metadata(&meta, "department");
    cr_assert_str_eq(dept, "Engineering", "Department should match");
    
    buckets_xl_meta_free(&meta);
}

Test(metadata, deserialize_with_versioning, .init = setup, .fini = teardown) {
    const char *json = "{"
        "\"version\": 1,"
        "\"format\": \"xl\","
        "\"stat\": {\"size\": 100, \"modTime\": \"2026-02-25T10:00:00Z\"},"
        "\"versioning\": {"
        "  \"versionId\": \"abc-123-def\","
        "  \"isLatest\": true,"
        "  \"isDeleteMarker\": false"
        "}"
    "}";
    
    buckets_xl_meta_t meta;
    int result = buckets_xl_meta_from_json(json, &meta);
    
    cr_assert_eq(result, 0, "Deserialization should succeed");
    cr_assert_str_eq(meta.versioning.versionId, "abc-123-def", "Version ID should match");
    cr_assert(meta.versioning.isLatest, "isLatest should be true");
    cr_assert(!meta.versioning.isDeleteMarker, "isDeleteMarker should be false");
    
    buckets_xl_meta_free(&meta);
}

Test(metadata, roundtrip_serialization, .init = setup, .fini = teardown) {
    /* Create metadata with all fields */
    buckets_xl_meta_t meta1 = {0};
    meta1.version = 1;
    strcpy(meta1.format, "xl");
    meta1.stat.size = 2048;
    strcpy(meta1.stat.modTime, "2026-02-25T12:00:00Z");
    
    meta1.meta.content_type = buckets_strdup("text/html");
    meta1.meta.cache_control = buckets_strdup("public, max-age=86400");
    meta1.meta.etag = buckets_strdup("abc123def456");
    
    buckets_add_user_metadata(&meta1, "owner", "Charlie");
    buckets_add_user_metadata(&meta1, "category", "documents");
    
    meta1.versioning.versionId = buckets_strdup("v1-test-version");
    meta1.versioning.isLatest = true;
    
    /* Serialize */
    char *json = buckets_xl_meta_to_json(&meta1);
    cr_assert_not_null(json, "Serialization should succeed");
    
    /* Deserialize */
    buckets_xl_meta_t meta2;
    int result = buckets_xl_meta_from_json(json, &meta2);
    cr_assert_eq(result, 0, "Deserialization should succeed");
    
    /* Verify all fields match */
    cr_assert_eq(meta2.version, 1, "Version should match");
    cr_assert_eq(meta2.stat.size, 2048, "Size should match");
    cr_assert_str_eq(meta2.meta.content_type, "text/html", "Content type should match");
    cr_assert_str_eq(meta2.meta.cache_control, "public, max-age=86400", "Cache control should match");
    cr_assert_str_eq(meta2.meta.etag, "abc123def456", "ETag should match");
    
    cr_assert_eq(meta2.meta.user_count, 2, "User metadata count should match");
    const char *owner = buckets_get_user_metadata(&meta2, "owner");
    cr_assert_str_eq(owner, "Charlie", "Owner should match");
    
    cr_assert_str_eq(meta2.versioning.versionId, "v1-test-version", "Version ID should match");
    cr_assert(meta2.versioning.isLatest, "isLatest should match");
    
    buckets_free(json);
    buckets_xl_meta_free(&meta1);
    buckets_xl_meta_free(&meta2);
}

/* ===== Integration Tests ===== */

Test(metadata, put_object_with_metadata, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "document.pdf";
    const char *data = "PDF document content here";
    size_t size = strlen(data);
    
    /* Prepare metadata */
    buckets_xl_meta_t meta = {0};
    meta.meta.content_type = buckets_strdup("application/pdf");
    meta.meta.cache_control = buckets_strdup("private");
    meta.meta.content_disposition = buckets_strdup("inline; filename=doc.pdf");
    
    buckets_add_user_metadata(&meta, "author", "Alice Smith");
    buckets_add_user_metadata(&meta, "department", "Legal");
    
    /* Write object */
    int result = buckets_put_object_with_metadata(bucket, object, data, size, 
                                                  &meta, false, NULL);
    cr_assert_eq(result, 0, "Put with metadata should succeed");
    
    /* Read back and verify */
    buckets_xl_meta_t read_meta;
    result = buckets_head_object(bucket, object, &read_meta);
    cr_assert_eq(result, 0, "Head should succeed");
    
    cr_assert_str_eq(read_meta.meta.content_type, "application/pdf", "Content type should match");
    cr_assert_str_eq(read_meta.meta.cache_control, "private", "Cache control should match");
    cr_assert_not_null(read_meta.meta.etag, "ETag should be computed");
    
    const char *author = buckets_get_user_metadata(&read_meta, "author");
    cr_assert_str_eq(author, "Alice Smith", "User metadata should match");
    
    buckets_xl_meta_free(&meta);
    buckets_xl_meta_free(&read_meta);
}

Test(metadata, put_object_with_versioning, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "versioned-doc.txt";
    const char *data = "Version 1 content";
    size_t size = strlen(data);
    
    char versionId[37];
    
    /* Write object with versioning */
    buckets_xl_meta_t meta = {0};
    int result = buckets_put_object_with_metadata(bucket, object, data, size,
                                                  &meta, true, versionId);
    cr_assert_eq(result, 0, "Put with versioning should succeed");
    cr_assert_eq(strlen(versionId), 36, "Version ID should be returned");
    
    /* Read back and verify versioning info */
    buckets_xl_meta_t read_meta;
    result = buckets_head_object(bucket, object, &read_meta);
    cr_assert_eq(result, 0, "Head should succeed");
    
    cr_assert_not_null(read_meta.versioning.versionId, "Version ID should be set");
    cr_assert(read_meta.versioning.isLatest, "Should be marked as latest");
    cr_assert(!read_meta.versioning.isDeleteMarker, "Should not be delete marker");
    
    buckets_xl_meta_free(&meta);
    buckets_xl_meta_free(&read_meta);
}

Test(metadata, etag_computed_automatically, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "auto-etag.txt";
    const char *data = "Content for automatic ETag";
    size_t size = strlen(data);
    
    buckets_xl_meta_t meta = {0};
    int result = buckets_put_object_with_metadata(bucket, object, data, size,
                                                  &meta, false, NULL);
    cr_assert_eq(result, 0, "Put should succeed");
    
    /* Read back and verify ETag was computed */
    buckets_xl_meta_t read_meta;
    result = buckets_head_object(bucket, object, &read_meta);
    cr_assert_eq(result, 0, "Head should succeed");
    
    cr_assert_not_null(read_meta.meta.etag, "ETag should be automatically computed");
    cr_assert_eq(strlen(read_meta.meta.etag), 64, "ETag should be 64 hex characters");
    
    buckets_xl_meta_free(&meta);
    buckets_xl_meta_free(&read_meta);
}
