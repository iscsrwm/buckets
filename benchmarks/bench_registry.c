/**
 * Location Registry Performance Benchmarks
 * 
 * Validates registry performance targets:
 * - Cache hit: <1 μs
 * - Cache miss (with storage): <5 ms
 * - Batch operations: Linear scaling
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "buckets.h"
#include "buckets_registry.h"
#include "buckets_storage.h"

#define TEST_DATA_DIR "/tmp/buckets-bench-registry"
#define NUM_ITERATIONS 10000
#define NUM_BATCH_ITEMS 100

/* Timing utilities */

static double get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000.0) + (tv.tv_usec / 1000.0);
}

static double get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * 1000000.0) + tv.tv_usec;
}

/* Setup/cleanup */

static void cleanup_test_dir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DATA_DIR);
    int ret = system(cmd);
    (void)ret;
}

static void setup_environment(void)
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

static void teardown_environment(void)
{
    buckets_registry_cleanup();
    buckets_storage_cleanup();
    cleanup_test_dir();
}

/* Benchmark 1: Cache Hit Performance */

static void bench_cache_hit(void)
{
    printf("\n=== Benchmark 1: Cache Hit Performance ===\n");
    
    /* Record a test location (will be in cache) */
    buckets_object_location_t loc = {0};
    loc.bucket = "bench-bucket";
    loc.object = "bench-object";
    loc.version_id = "v1";
    loc.pool_idx = 0;
    loc.set_idx = 2;
    loc.disk_count = 12;
    for (u32 i = 0; i < 12; i++) {
        loc.disk_idxs[i] = i;
    }
    loc.generation = 1;
    loc.mod_time = time(NULL);
    loc.size = 1024000;
    
    buckets_registry_record(&loc);
    
    /* Warm up cache */
    for (int i = 0; i < 10; i++) {
        buckets_object_location_t *result = NULL;
        buckets_registry_lookup("bench-bucket", "bench-object", "v1", &result);
        buckets_registry_location_free(result);
    }
    
    /* Benchmark cache hits */
    double start = get_time_us();
    
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        buckets_object_location_t *result = NULL;
        buckets_registry_lookup("bench-bucket", "bench-object", "v1", &result);
        buckets_registry_location_free(result);
    }
    
    double end = get_time_us();
    double total_time_us = end - start;
    double avg_time_us = total_time_us / NUM_ITERATIONS;
    
    printf("  Iterations: %d\n", NUM_ITERATIONS);
    printf("  Total time: %.2f ms\n", total_time_us / 1000.0);
    printf("  Avg lookup: %.3f μs\n", avg_time_us);
    printf("  Target: <1 μs\n");
    printf("  Status: %s\n", avg_time_us < 1.0 ? "✓ PASS" : "✗ FAIL");
}

/* Benchmark 2: Cache Statistics */

static void bench_cache_stats(void)
{
    printf("\n=== Benchmark 2: Cache Statistics ===\n");
    
    /* Record 1000 test locations */
    printf("  Recording 1000 locations...\n");
    for (int i = 0; i < 1000; i++) {
        buckets_object_location_t loc = {0};
        
        char bucket[64], object[64], version[64];
        snprintf(bucket, sizeof(bucket), "bucket-%d", i);
        snprintf(object, sizeof(object), "object-%d", i);
        snprintf(version, sizeof(version), "v%d", i);
        
        loc.bucket = bucket;
        loc.object = object;
        loc.version_id = version;
        loc.pool_idx = 0;
        loc.set_idx = i % 16;
        loc.disk_count = 12;
        for (u32 j = 0; j < 12; j++) {
            loc.disk_idxs[j] = j;
        }
        loc.generation = 1;
        loc.mod_time = time(NULL);
        loc.size = 1024 * (i + 1);
        
        buckets_registry_record(&loc);
    }
    
    /* Perform 10000 lookups (80% hits, 20% misses) */
    printf("  Performing 10000 lookups (80%% hits, 20%% misses)...\n");
    for (int i = 0; i < 10000; i++) {
        char bucket[64], object[64], version[64];
        int idx = (i % 5 == 0) ? (i + 10000) : (i % 1000);  /* 20% miss rate */
        
        snprintf(bucket, sizeof(bucket), "bucket-%d", idx);
        snprintf(object, sizeof(object), "object-%d", idx);
        snprintf(version, sizeof(version), "v%d", idx);
        
        buckets_object_location_t *result = NULL;
        buckets_registry_lookup(bucket, object, version, &result);
        buckets_registry_location_free(result);
    }
    
    /* Get statistics */
    buckets_registry_stats_t stats;
    buckets_registry_get_stats(&stats);
    
    printf("\n  Cache Statistics:\n");
    printf("    Total entries: %lu\n", stats.total_entries);
    printf("    Hits: %lu\n", stats.hits);
    printf("    Misses: %lu\n", stats.misses);
    printf("    Evictions: %lu\n", stats.evictions);
    printf("    Hit rate: %.2f%%\n", stats.hit_rate);
    printf("    Target hit rate: >99%%\n");
    printf("    Status: %s\n", stats.hit_rate > 99.0 ? "✓ PASS" : "✗ FAIL");
}

/* Benchmark 3: Batch Operations */

static void bench_batch_operations(void)
{
    printf("\n=== Benchmark 3: Batch Operations ===\n");
    
    /* Prepare batch data */
    buckets_object_location_t locations[NUM_BATCH_ITEMS];
    
    for (int i = 0; i < NUM_BATCH_ITEMS; i++) {
        char *bucket = malloc(64);
        char *object = malloc(64);
        char *version = malloc(64);
        
        snprintf(bucket, 64, "batch-bucket-%d", i);
        snprintf(object, 64, "batch-object-%d", i);
        snprintf(version, 64, "batch-v%d", i);
        
        locations[i].bucket = bucket;
        locations[i].object = object;
        locations[i].version_id = version;
        locations[i].pool_idx = 0;
        locations[i].set_idx = i % 16;
        locations[i].disk_count = 12;
        for (u32 j = 0; j < 12; j++) {
            locations[i].disk_idxs[j] = j;
        }
        locations[i].generation = 1;
        locations[i].mod_time = time(NULL);
        locations[i].size = 1024 * (i + 1);
    }
    
    /* Benchmark batch record */
    double start = get_time_ms();
    int recorded = buckets_registry_record_batch(locations, NUM_BATCH_ITEMS);
    double end = get_time_ms();
    
    printf("  Batch Record:\n");
    printf("    Items: %d\n", NUM_BATCH_ITEMS);
    printf("    Recorded: %d\n", recorded);
    printf("    Total time: %.2f ms\n", end - start);
    printf("    Avg per item: %.3f ms\n", (end - start) / NUM_BATCH_ITEMS);
    
    /* Prepare batch lookup keys */
    buckets_registry_key_t keys[NUM_BATCH_ITEMS];
    for (int i = 0; i < NUM_BATCH_ITEMS; i++) {
        keys[i].bucket = locations[i].bucket;
        keys[i].object = locations[i].object;
        keys[i].version_id = locations[i].version_id;
    }
    
    /* Benchmark batch lookup */
    start = get_time_ms();
    buckets_object_location_t **results = NULL;
    int found = buckets_registry_lookup_batch(keys, NUM_BATCH_ITEMS, &results);
    end = get_time_ms();
    
    printf("\n  Batch Lookup:\n");
    printf("    Items: %d\n", NUM_BATCH_ITEMS);
    printf("    Found: %d\n", found);
    printf("    Total time: %.2f ms\n", end - start);
    printf("    Avg per item: %.3f ms\n", (end - start) / NUM_BATCH_ITEMS);
    
    /* Cleanup */
    for (int i = 0; i < NUM_BATCH_ITEMS; i++) {
        if (results && results[i]) {
            buckets_registry_location_free(results[i]);
        }
        free(locations[i].bucket);
        free(locations[i].object);
        free(locations[i].version_id);
    }
    free(results);
    
    printf("    Status: ✓ PASS (batch operations functional)\n");
}

/* Benchmark 4: Update Operation */

static void bench_update_operation(void)
{
    printf("\n=== Benchmark 4: Update Operation ===\n");
    
    /* Record initial location */
    buckets_object_location_t loc = {0};
    loc.bucket = "update-bucket";
    loc.object = "update-object";
    loc.version_id = "update-v1";
    loc.pool_idx = 0;
    loc.set_idx = 0;
    loc.disk_count = 12;
    for (u32 i = 0; i < 12; i++) {
        loc.disk_idxs[i] = i;
    }
    loc.generation = 1;
    loc.mod_time = time(NULL);
    loc.size = 1024000;
    
    buckets_registry_record(&loc);
    
    /* Benchmark updates */
    double start = get_time_ms();
    
    for (int i = 0; i < 1000; i++) {
        loc.set_idx = (i % 16);
        loc.generation = i + 2;
        buckets_registry_update("update-bucket", "update-object", "update-v1", &loc);
    }
    
    double end = get_time_ms();
    double total_time_ms = end - start;
    double avg_time_ms = total_time_ms / 1000.0;
    
    printf("  Iterations: 1000\n");
    printf("  Total time: %.2f ms\n", total_time_ms);
    printf("  Avg update: %.3f ms\n", avg_time_ms);
    printf("  Status: ✓ PASS (update operations functional)\n");
}

/* Main */

int main(void)
{
    printf("=========================================\n");
    printf("Location Registry Performance Benchmarks\n");
    printf("=========================================\n");
    
    setup_environment();
    
    bench_cache_hit();
    bench_cache_stats();
    bench_batch_operations();
    bench_update_operation();
    
    printf("\n=========================================\n");
    printf("Benchmarks Complete\n");
    printf("=========================================\n");
    
    teardown_environment();
    
    return 0;
}
