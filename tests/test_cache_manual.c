/**
 * Manual Cache Tests
 * 
 * Simple tests to verify cache initialization and basic operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "buckets.h"
#include "buckets_cache.h"
#include "buckets_cluster.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("TEST: %s ... ", name); \
        tests_run++; \
    } while (0)

#define PASS() \
    do { \
        printf("PASS\n"); \
        tests_passed++; \
    } while (0)

#define FAIL(msg) \
    do { \
        printf("FAIL: %s\n", msg); \
        return; \
    } while (0)

void test_cache_init_cleanup(void) {
    TEST("Cache initialization and cleanup");
    
    buckets_format_cache_init();
    buckets_topology_cache_init();
    
    /* Should start empty */
    assert(buckets_format_cache_get() == NULL);
    assert(buckets_topology_cache_get() == NULL);
    
    buckets_topology_cache_cleanup();
    buckets_format_cache_cleanup();
    
    PASS();
}

void test_format_cache_operations(void) {
    TEST("Format cache set/get/invalidate");
    
    buckets_format_cache_init();
    
    /* Create a format */
    buckets_format_t *format = buckets_format_new(2, 4);
    assert(format != NULL);
    strcpy(format->meta.deployment_id, "test-deployment-uuid-12345678901");
    
    /* Cache it */
    int ret = buckets_format_cache_set(format);
    assert(ret == BUCKETS_OK);
    
    /* Retrieve from cache */
    buckets_format_t *cached = buckets_format_cache_get();
    assert(cached != NULL);
    assert(strcmp(cached->meta.version, "1") == 0);
    assert(strcmp(cached->meta.deployment_id, "test-deployment-uuid-12345678901") == 0);
    
    /* Invalidate */
    buckets_format_cache_invalidate();
    assert(buckets_format_cache_get() == NULL);
    
    buckets_format_free(format);
    buckets_format_cache_cleanup();
    
    PASS();
}

void test_topology_cache_operations(void) {
    TEST("Topology cache set/get/invalidate");
    
    buckets_topology_cache_init();
    
    /* Create a format first (needed for topology) */
    buckets_format_t *format = buckets_format_new(2, 4);
    assert(format != NULL);
    strcpy(format->meta.deployment_id, "test-deployment-uuid-12345678901");
    
    /* Create topology from format */
    buckets_cluster_topology_t *topology = buckets_topology_from_format(format);
    assert(topology != NULL);
    assert(topology->generation == 1);
    
    /* Cache it (transfers ownership) */
    int ret = buckets_topology_cache_set(topology);
    assert(ret == BUCKETS_OK);
    
    /* Retrieve from cache */
    buckets_cluster_topology_t *cached = buckets_topology_cache_get();
    assert(cached != NULL);
    assert(cached->generation == 1);
    assert(strcmp(cached->deployment_id, "test-deployment-uuid-12345678901") == 0);
    
    /* Invalidate */
    buckets_topology_cache_invalidate();
    assert(buckets_topology_cache_get() == NULL);
    
    buckets_format_free(format);
    buckets_topology_cache_cleanup();
    
    PASS();
}

void test_buckets_init_cleanup(void) {
    TEST("buckets_init() initializes caches");
    
    /* Initialize buckets (should init caches) */
    int ret = buckets_init();
    assert(ret == 0);
    
    /* Caches should be initialized (empty but usable) */
    assert(buckets_format_cache_get() == NULL);
    assert(buckets_topology_cache_get() == NULL);
    
    /* Should be able to use caches */
    buckets_format_t *format = buckets_format_new(2, 4);
    assert(format != NULL);
    
    ret = buckets_format_cache_set(format);
    assert(ret == BUCKETS_OK);
    
    buckets_format_t *cached = buckets_format_cache_get();
    assert(cached != NULL);
    
    buckets_format_free(format);
    
    /* Cleanup (should cleanup caches) */
    buckets_cleanup();
    
    PASS();
}

int main(void) {
    printf("=== Manual Cache Tests ===\n\n");
    
    test_cache_init_cleanup();
    test_format_cache_operations();
    test_topology_cache_operations();
    test_buckets_init_cleanup();
    
    printf("\n=== Summary ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_run - tests_passed);
    
    return (tests_run == tests_passed) ? 0 : 1;
}
