/**
 * Simple manual test for versioning functionality
 * 
 * This is a quick sanity check - full Criterion test suite can be added later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "buckets.h"
#include "buckets_storage.h"

int main(void)
{
    printf("=== Buckets Versioning Simple Test ===\n\n");
    
    /* Initialize buckets */
    buckets_init();
    
    /* Initialize storage */
    buckets_storage_config_t config = {
        .data_dir = "/tmp/buckets-versioning-test",
        .inline_threshold = 128 * 1024,
        .default_ec_k = 8,
        .default_ec_m = 4,
        .verify_checksums = true
    };
    
    if (buckets_storage_init(&config) != 0) {
        fprintf(stderr, "Failed to initialize storage\n");
        return 1;
    }
    
    /* Initialize metadata cache */
    if (buckets_metadata_cache_init(1000, 300) != 0) {
        fprintf(stderr, "Failed to initialize metadata cache\n");
        return 1;
    }
    
    printf("✓ Initialized storage and cache\n");
    
    /* Test 1: Generate version ID */
    printf("\nTest 1: Version ID generation\n");
    char version_id[37];
    if (buckets_generate_version_id(version_id) == 0) {
        printf("  Generated version ID: %s\n", version_id);
        assert(strlen(version_id) == 36);  /* UUID format */
        printf("✓ Version ID generation works\n");
    } else {
        fprintf(stderr, "✗ Version ID generation failed\n");
        return 1;
    }
    
    /* Test 2: ETag computation */
    printf("\nTest 2: ETag computation\n");
    const char *test_data = "Hello, World!";
    char etag[65];
    if (buckets_compute_etag(test_data, strlen(test_data), etag) == 0) {
        printf("  ETag: %s\n", etag);
        assert(strlen(etag) == 64);  /* BLAKE2b-256 hex = 64 chars */
        printf("✓ ETag computation works\n");
    } else {
        fprintf(stderr, "✗ ETag computation failed\n");
        return 1;
    }
    
    /* Test 3: User metadata */
    printf("\nTest 3: User metadata\n");
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    
    if (buckets_add_user_metadata(&meta, "author", "John Doe") == 0) {
        const char *value = buckets_get_user_metadata(&meta, "author");
        if (value && strcmp(value, "John Doe") == 0) {
            printf("  User metadata: author=%s\n", value);
            printf("✓ User metadata works\n");
        } else {
            fprintf(stderr, "✗ User metadata retrieval failed\n");
            return 1;
        }
    } else {
        fprintf(stderr, "✗ User metadata addition failed\n");
        return 1;
    }
    
    buckets_xl_meta_free(&meta);
    
    /* Test 4: Metadata cache */
    printf("\nTest 4: Metadata cache\n");
    buckets_xl_meta_t cache_meta = {0};
    cache_meta.version = 1;
    strcpy(cache_meta.format, "xl");
    cache_meta.stat.size = 1024;
    strcpy(cache_meta.stat.modTime, "2026-02-26T00:00:00Z");
    
    /* Put in cache */
    if (buckets_metadata_cache_put("mybucket", "myobject", NULL, &cache_meta) == 0) {
        printf("  Cached metadata for mybucket/myobject\n");
        
        /* Get from cache */
        buckets_xl_meta_t retrieved_meta = {0};
        if (buckets_metadata_cache_get("mybucket", "myobject", NULL, &retrieved_meta) == 0) {
            printf("  Retrieved from cache: size=%zu, modTime=%s\n",
                   retrieved_meta.stat.size, retrieved_meta.stat.modTime);
            assert(retrieved_meta.stat.size == 1024);
            printf("✓ Metadata cache works\n");
            buckets_xl_meta_free(&retrieved_meta);
        } else {
            fprintf(stderr, "✗ Cache retrieval failed\n");
            return 1;
        }
    } else {
        fprintf(stderr, "✗ Cache put failed\n");
        return 1;
    }
    
    /* Test 5: Cache statistics */
    printf("\nTest 5: Cache statistics\n");
    u64 hits, misses, evictions;
    u32 count;
    buckets_metadata_cache_stats(&hits, &misses, &evictions, &count);
    printf("  Cache stats: hits=%llu, misses=%llu, evictions=%llu, count=%u\n",
           (unsigned long long)hits, (unsigned long long)misses,
           (unsigned long long)evictions, count);
    assert(hits >= 1);  /* At least one hit from Test 4 */
    printf("✓ Cache statistics work\n");
    
    /* Cleanup */
    buckets_metadata_cache_cleanup();
    buckets_storage_cleanup();
    buckets_cleanup();
    
    printf("\n=== All Tests Passed! ===\n");
    return 0;
}
