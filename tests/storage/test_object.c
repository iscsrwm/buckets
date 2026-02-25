/**
 * Criterion Unit Tests for Storage Layer (Object Operations)
 * 
 * Comprehensive test suite for object.c operations:
 * - Storage initialization/cleanup
 * - Inline object write/read (<128KB)
 * - Large object write/read with erasure coding
 * - Object delete operations
 * - Head/stat operations
 * - Checksum verification
 */

#include <criterion/criterion.h>
#include <criterion/redirect.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buckets.h"
#include "buckets_storage.h"

/* Test fixtures */
static char test_data_dir[PATH_MAX];

void setup(void) {
    buckets_log_init();
    buckets_set_log_level(BUCKETS_LOG_ERROR);  /* Quiet during tests */
    
    /* Create temporary test directory */
    snprintf(test_data_dir, sizeof(test_data_dir), "/tmp/buckets_test_%d", getpid());
    mkdir(test_data_dir, 0755);
    
    /* Initialize storage */
    buckets_storage_config_t config = {
        .data_dir = test_data_dir,
        .inline_threshold = 128 * 1024,  /* 128KB */
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
    (void)ret;  /* Ignore cleanup errors */
}

/* ===== Storage Initialization Tests ===== */

Test(storage, init_with_valid_config, .init = setup, .fini = teardown) {
    /* Already initialized in setup */
    const buckets_storage_config_t *config = buckets_storage_get_config();
    
    cr_assert_not_null(config, "Config should not be NULL");
    cr_assert_str_eq(config->data_dir, test_data_dir, "Data dir should match");
    cr_assert_eq(config->inline_threshold, 128 * 1024, "Inline threshold should be 128KB");
    cr_assert_eq(config->default_ec_k, 8, "Default K should be 8");
    cr_assert_eq(config->default_ec_m, 4, "Default M should be 4");
    cr_assert(config->verify_checksums, "Checksums should be enabled");
}

Test(storage, init_with_null_config) {
    int result = buckets_storage_init(NULL);
    cr_assert_eq(result, -1, "Init with NULL config should fail");
}

/* ===== Inline Object Tests (<128KB) ===== */

Test(storage, put_and_get_small_object, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "small-file.txt";
    const char *data = "Hello, Buckets! This is a small test file.";
    size_t size = strlen(data);
    
    /* Write object */
    int result = buckets_put_object(bucket, object, data, size, "text/plain");
    cr_assert_eq(result, 0, "Put object should succeed");
    
    /* Read object back */
    void *read_data = NULL;
    size_t read_size = 0;
    result = buckets_get_object(bucket, object, &read_data, &read_size);
    cr_assert_eq(result, 0, "Get object should succeed");
    cr_assert_not_null(read_data, "Read data should not be NULL");
    cr_assert_eq(read_size, size, "Read size should match write size");
    cr_assert(memcmp(read_data, data, size) == 0, "Read data should match write data");
    
    buckets_free(read_data);
}

Test(storage, put_inline_object_with_metadata, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "doc.pdf";
    u8 data[1024];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (u8)(i % 256);
    }
    
    /* Write object with content type */
    int result = buckets_put_object(bucket, object, data, sizeof(data), "application/pdf");
    cr_assert_eq(result, 0, "Put object should succeed");
    
    /* Check metadata */
    buckets_xl_meta_t meta;
    result = buckets_head_object(bucket, object, &meta);
    cr_assert_eq(result, 0, "Head object should succeed");
    cr_assert_eq(meta.stat.size, sizeof(data), "Size should match");
    cr_assert_str_eq(meta.meta.content_type, "application/pdf", "Content type should match");
    cr_assert_not_null(meta.inline_data, "Inline data should be present");
    
    buckets_xl_meta_free(&meta);
}

Test(storage, inline_size_threshold, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    
    /* Test object just below threshold (should inline) */
    size_t below_threshold = 127 * 1024;
    u8 *small_data = buckets_malloc(below_threshold);
    memset(small_data, 0xAA, below_threshold);
    
    int result = buckets_put_object(bucket, "just-below", small_data, below_threshold, NULL);
    cr_assert_eq(result, 0, "Put below threshold should succeed");
    
    buckets_xl_meta_t meta1;
    result = buckets_head_object(bucket, "just-below", &meta1);
    cr_assert_eq(result, 0, "Head should succeed");
    cr_assert_not_null(meta1.inline_data, "Should be inlined");
    buckets_xl_meta_free(&meta1);
    
    /* Test object just above threshold (should NOT inline) */
    size_t above_threshold = 129 * 1024;
    u8 *large_data = buckets_malloc(above_threshold);
    memset(large_data, 0xBB, above_threshold);
    
    result = buckets_put_object(bucket, "just-above", large_data, above_threshold, NULL);
    cr_assert_eq(result, 0, "Put above threshold should succeed");
    
    buckets_xl_meta_t meta2;
    result = buckets_head_object(bucket, "just-above", &meta2);
    cr_assert_eq(result, 0, "Head should succeed");
    cr_assert_null(meta2.inline_data, "Should NOT be inlined");
    cr_assert_eq(meta2.erasure.data, 8, "Should have 8 data chunks");
    cr_assert_eq(meta2.erasure.parity, 4, "Should have 4 parity chunks");
    buckets_xl_meta_free(&meta2);
    
    buckets_free(small_data);
    buckets_free(large_data);
}

/* ===== Large Object Tests (Erasure Coding) ===== */

Test(storage, put_and_get_large_object, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "large-file.bin";
    
    /* Create 1MB test data with pattern */
    size_t size = 1024 * 1024;
    u8 *data = buckets_malloc(size);
    for (size_t i = 0; i < size; i++) {
        data[i] = (u8)(i % 256);
    }
    
    /* Write object */
    int result = buckets_put_object(bucket, object, data, size, "application/octet-stream");
    cr_assert_eq(result, 0, "Put large object should succeed");
    
    /* Read object back */
    void *read_data = NULL;
    size_t read_size = 0;
    result = buckets_get_object(bucket, object, &read_data, &read_size);
    cr_assert_eq(result, 0, "Get large object should succeed");
    cr_assert_not_null(read_data, "Read data should not be NULL");
    cr_assert_eq(read_size, size, "Read size should match write size");
    cr_assert(memcmp(read_data, data, size) == 0, "Read data should match write data");
    
    buckets_free(data);
    buckets_free(read_data);
}

Test(storage, large_object_has_chunks, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "chunked-file.dat";
    
    /* Create 512KB test data */
    size_t size = 512 * 1024;
    u8 *data = buckets_malloc(size);
    memset(data, 0x55, size);
    
    /* Write object */
    int result = buckets_put_object(bucket, object, data, size, NULL);
    cr_assert_eq(result, 0, "Put should succeed");
    
    /* Check metadata */
    buckets_xl_meta_t meta;
    result = buckets_head_object(bucket, object, &meta);
    cr_assert_eq(result, 0, "Head should succeed");
    
    cr_assert_null(meta.inline_data, "Should NOT be inlined");
    cr_assert_eq(meta.erasure.data, 8, "Should have 8 data chunks");
    cr_assert_eq(meta.erasure.parity, 4, "Should have 4 parity chunks");
    cr_assert_str_eq(meta.erasure.algorithm, "ReedSolomon", "Algorithm should be ReedSolomon");
    cr_assert_gt(meta.erasure.blockSize, 0, "Block size should be positive");
    
    /* Verify checksums exist */
    cr_assert_not_null(meta.erasure.checksums, "Checksums should exist");
    for (u32 i = 0; i < 12; i++) {  /* 8 data + 4 parity */
        cr_assert_str_eq(meta.erasure.checksums[i].algo, "BLAKE2b-256", 
                        "Checksum algo should be BLAKE2b-256");
        cr_assert_gt(strlen((const char*)meta.erasure.checksums[i].hash), 0, 
                    "Checksum hash should not be empty");
    }
    
    buckets_xl_meta_free(&meta);
    buckets_free(data);
}

Test(storage, multiple_large_objects, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    size_t size = 256 * 1024;
    
    /* Write multiple objects */
    for (int i = 0; i < 5; i++) {
        char object_name[64];
        snprintf(object_name, sizeof(object_name), "file-%d.bin", i);
        
        u8 *data = buckets_malloc(size);
        /* Each file has different pattern */
        for (size_t j = 0; j < size; j++) {
            data[j] = (u8)((i * 100 + j) % 256);
        }
        
        int result = buckets_put_object(bucket, object_name, data, size, NULL);
        cr_assert_eq(result, 0, "Put object %d should succeed", i);
        
        buckets_free(data);
    }
    
    /* Verify we can read all objects back correctly */
    for (int i = 0; i < 5; i++) {
        char object_name[64];
        snprintf(object_name, sizeof(object_name), "file-%d.bin", i);
        
        void *read_data = NULL;
        size_t read_size = 0;
        int result = buckets_get_object(bucket, object_name, &read_data, &read_size);
        cr_assert_eq(result, 0, "Get object %d should succeed", i);
        cr_assert_eq(read_size, size, "Size should match");
        
        /* Verify pattern */
        u8 *bytes = (u8*)read_data;
        for (size_t j = 0; j < size; j++) {
            cr_assert_eq(bytes[j], (u8)((i * 100 + j) % 256), 
                        "Data pattern should match for object %d", i);
        }
        
        buckets_free(read_data);
    }
}

/* ===== Object Delete Tests ===== */

Test(storage, delete_inline_object, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "delete-me-small.txt";
    const char *data = "This will be deleted";
    
    /* Write object */
    int result = buckets_put_object(bucket, object, data, strlen(data), NULL);
    cr_assert_eq(result, 0, "Put should succeed");
    
    /* Verify it exists */
    void *read_data = NULL;
    size_t read_size = 0;
    result = buckets_get_object(bucket, object, &read_data, &read_size);
    cr_assert_eq(result, 0, "Get should succeed before delete");
    buckets_free(read_data);
    
    /* Delete object */
    result = buckets_delete_object(bucket, object);
    cr_assert_eq(result, 0, "Delete should succeed");
    
    /* Verify it no longer exists */
    read_data = NULL;
    result = buckets_get_object(bucket, object, &read_data, &read_size);
    cr_assert_eq(result, -1, "Get should fail after delete");
}

Test(storage, delete_large_object, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "delete-me-large.bin";
    
    /* Create 512KB object */
    size_t size = 512 * 1024;
    u8 *data = buckets_malloc(size);
    memset(data, 0x77, size);
    
    /* Write object */
    int result = buckets_put_object(bucket, object, data, size, NULL);
    cr_assert_eq(result, 0, "Put should succeed");
    buckets_free(data);
    
    /* Delete object */
    result = buckets_delete_object(bucket, object);
    cr_assert_eq(result, 0, "Delete should succeed");
    
    /* Verify it no longer exists */
    void *read_data = NULL;
    size_t read_size = 0;
    result = buckets_get_object(bucket, object, &read_data, &read_size);
    cr_assert_eq(result, -1, "Get should fail after delete");
}

Test(storage, delete_nonexistent_object, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "does-not-exist.txt";
    
    /* Try to delete non-existent object */
    int result = buckets_delete_object(bucket, object);
    cr_assert_eq(result, -1, "Delete of non-existent object should fail");
}

/* ===== Head/Stat Tests ===== */

Test(storage, head_object_returns_metadata, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "metadata-test.txt";
    const char *data = "Test data for metadata";
    size_t size = strlen(data);
    
    /* Write object */
    int result = buckets_put_object(bucket, object, data, size, "text/plain");
    cr_assert_eq(result, 0, "Put should succeed");
    
    /* Head object */
    buckets_xl_meta_t meta;
    result = buckets_head_object(bucket, object, &meta);
    cr_assert_eq(result, 0, "Head should succeed");
    
    cr_assert_eq(meta.version, 1, "Version should be 1");
    cr_assert_str_eq(meta.format, "xl", "Format should be xl");
    cr_assert_eq(meta.stat.size, size, "Size should match");
    cr_assert_str_eq(meta.meta.content_type, "text/plain", "Content type should match");
    cr_assert_gt(strlen(meta.stat.modTime), 0, "modTime should be set");
    
    buckets_xl_meta_free(&meta);
}

Test(storage, stat_object_returns_size_and_time, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "stat-test.bin";
    
    /* Create test data */
    size_t size = 4096;
    u8 *data = buckets_malloc(size);
    memset(data, 0xCC, size);
    
    /* Write object */
    int result = buckets_put_object(bucket, object, data, size, NULL);
    cr_assert_eq(result, 0, "Put should succeed");
    buckets_free(data);
    
    /* Stat object */
    size_t stat_size;
    char mod_time[64];
    result = buckets_stat_object(bucket, object, &stat_size, mod_time);
    cr_assert_eq(result, 0, "Stat should succeed");
    cr_assert_eq(stat_size, size, "Stat size should match");
    cr_assert_gt(strlen(mod_time), 0, "modTime should be set");
}

/* ===== Error Handling Tests ===== */

Test(storage, put_with_null_parameters) {
    int result;
    const char *data = "test";
    
    result = buckets_put_object(NULL, "object", data, 4, NULL);
    cr_assert_eq(result, -1, "Put with NULL bucket should fail");
    
    result = buckets_put_object("bucket", NULL, data, 4, NULL);
    cr_assert_eq(result, -1, "Put with NULL object should fail");
    
    result = buckets_put_object("bucket", "object", NULL, 4, NULL);
    cr_assert_eq(result, -1, "Put with NULL data should fail");
}

Test(storage, get_with_null_parameters) {
    int result;
    void *data = NULL;
    size_t size = 0;
    
    result = buckets_get_object(NULL, "object", &data, &size);
    cr_assert_eq(result, -1, "Get with NULL bucket should fail");
    
    result = buckets_get_object("bucket", NULL, &data, &size);
    cr_assert_eq(result, -1, "Get with NULL object should fail");
    
    result = buckets_get_object("bucket", "object", NULL, &size);
    cr_assert_eq(result, -1, "Get with NULL data pointer should fail");
    
    result = buckets_get_object("bucket", "object", &data, NULL);
    cr_assert_eq(result, -1, "Get with NULL size pointer should fail");
}

Test(storage, get_nonexistent_object, .init = setup, .fini = teardown) {
    void *data = NULL;
    size_t size = 0;
    
    int result = buckets_get_object("bucket", "nonexistent.txt", &data, &size);
    cr_assert_eq(result, -1, "Get of non-existent object should fail");
}

/* ===== Overwrite Tests ===== */

Test(storage, overwrite_inline_object, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "overwrite-test.txt";
    
    /* Write initial object */
    const char *data1 = "Original content";
    int result = buckets_put_object(bucket, object, data1, strlen(data1), NULL);
    cr_assert_eq(result, 0, "First put should succeed");
    
    /* Overwrite with new content */
    const char *data2 = "New content that is different";
    result = buckets_put_object(bucket, object, data2, strlen(data2), NULL);
    cr_assert_eq(result, 0, "Second put should succeed");
    
    /* Read and verify we get new content */
    void *read_data = NULL;
    size_t read_size = 0;
    result = buckets_get_object(bucket, object, &read_data, &read_size);
    cr_assert_eq(result, 0, "Get should succeed");
    cr_assert_eq(read_size, strlen(data2), "Size should match new data");
    cr_assert(memcmp(read_data, data2, strlen(data2)) == 0, "Should read new data");
    
    buckets_free(read_data);
}

Test(storage, overwrite_large_object, .init = setup, .fini = teardown) {
    const char *bucket = "testbucket";
    const char *object = "overwrite-large.bin";
    size_t size = 256 * 1024;
    
    /* Write initial object */
    u8 *data1 = buckets_malloc(size);
    memset(data1, 0xAA, size);
    int result = buckets_put_object(bucket, object, data1, size, NULL);
    cr_assert_eq(result, 0, "First put should succeed");
    buckets_free(data1);
    
    /* Overwrite with different content */
    u8 *data2 = buckets_malloc(size);
    memset(data2, 0xBB, size);
    result = buckets_put_object(bucket, object, data2, size, NULL);
    cr_assert_eq(result, 0, "Second put should succeed");
    buckets_free(data2);
    
    /* Read and verify we get new content */
    void *read_data = NULL;
    size_t read_size = 0;
    result = buckets_get_object(bucket, object, &read_data, &read_size);
    cr_assert_eq(result, 0, "Get should succeed");
    
    u8 *bytes = (u8*)read_data;
    for (size_t i = 0; i < size; i++) {
        cr_assert_eq(bytes[i], 0xBB, "Should read new data pattern");
    }
    
    buckets_free(read_data);
}
