/**
 * Topology Integration Tests
 * 
 * End-to-end tests for the complete topology management system:
 * - Full cluster lifecycle (init → load → changes → persist)
 * - Multi-step topology evolution scenarios
 * - Failure recovery and healing
 * - Performance validation (<1 second changes)
 * - Concurrent operations
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_cache.h"

/* Test fixture */
static char test_dir[256];
static char *disk_paths[8];
static int disk_count = 8;

/* Performance tracking */
static struct timeval perf_start, perf_end;

#define PERF_START() gettimeofday(&perf_start, NULL)
#define PERF_END() gettimeofday(&perf_end, NULL)
#define PERF_MS() ((perf_end.tv_sec - perf_start.tv_sec) * 1000.0 + \
                   (perf_end.tv_usec - perf_start.tv_usec) / 1000.0)

void setup_integration(void) {
    buckets_init();
    buckets_topology_cache_init();
    
    snprintf(test_dir, sizeof(test_dir), "/tmp/buckets_test_integration_%d_%ld",
             getpid(), (long)time(NULL));
    mkdir(test_dir, 0755);
    
    for (int i = 0; i < disk_count; i++) {
        disk_paths[i] = buckets_format("%s/disk%d", test_dir, i);
        mkdir(disk_paths[i], 0755);
    }
}

void teardown_integration(void) {
    buckets_topology_manager_cleanup();
    buckets_topology_cache_cleanup();
    
    for (int i = 0; i < disk_count; i++) {
        if (disk_paths[i]) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "rm -rf %s", disk_paths[i]);
            if (system(cmd) != 0) { /* Ignore */ }
            buckets_free(disk_paths[i]);
            disk_paths[i] = NULL;
        }
    }
    
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    if (system(cmd) != 0) { /* Ignore */ }
    
    buckets_cleanup();
}

TestSuite(topology_integration, .init = setup_integration, .fini = teardown_integration);

/* ===================================================================
 * Full Cluster Lifecycle Tests
 * ===================================================================*/

Test(topology_integration, full_cluster_lifecycle) {
    /* Step 1: Initialize manager */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK, "Manager init should succeed");
    
    /* Step 2: Create initial topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "integration-test-cluster");
    topology->generation = 0;
    
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Step 3: Add first pool */
    ret = buckets_topology_manager_add_pool();
    cr_assert_eq(ret, BUCKETS_OK, "Add pool 0 should succeed");
    
    buckets_cluster_topology_t *current = buckets_topology_manager_get();
    cr_assert_eq(current->generation, 1);
    cr_assert_eq(current->pool_count, 1);
    
    /* Step 4: Add erasure sets to pool */
    buckets_disk_info_t set1_disks[4] = {
        {.endpoint = "http://node1:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node1:9000/disk2", .capacity = 1000000000},
        {.endpoint = "http://node2:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node2:9000/disk2", .capacity = 1000000000}
    };
    
    ret = buckets_topology_manager_add_set(0, set1_disks, 4);
    cr_assert_eq(ret, BUCKETS_OK, "Add set 0 should succeed");
    
    current = buckets_topology_manager_get();
    cr_assert_eq(current->generation, 2);
    cr_assert_eq(current->pools[0].set_count, 1);
    
    /* Add second set */
    buckets_disk_info_t set2_disks[4] = {
        {.endpoint = "http://node3:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node3:9000/disk2", .capacity = 1000000000},
        {.endpoint = "http://node4:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node4:9000/disk2", .capacity = 1000000000}
    };
    
    ret = buckets_topology_manager_add_set(0, set2_disks, 4);
    cr_assert_eq(ret, BUCKETS_OK, "Add set 1 should succeed");
    
    current = buckets_topology_manager_get();
    cr_assert_eq(current->generation, 3);
    cr_assert_eq(current->pools[0].set_count, 2);
    
    /* Step 5: Simulate disk failure and set draining */
    ret = buckets_topology_manager_mark_set_draining(0, 1);
    cr_assert_eq(ret, BUCKETS_OK, "Mark set draining should succeed");
    
    current = buckets_topology_manager_get();
    cr_assert_eq(current->generation, 4);
    cr_assert_eq(current->pools[0].sets[1].state, SET_STATE_DRAINING);
    
    /* Step 6: Complete migration and mark removed */
    ret = buckets_topology_manager_mark_set_removed(0, 1);
    cr_assert_eq(ret, BUCKETS_OK, "Mark set removed should succeed");
    
    current = buckets_topology_manager_get();
    cr_assert_eq(current->generation, 5);
    cr_assert_eq(current->pools[0].sets[1].state, SET_STATE_REMOVED);
    
    /* Step 7: Verify persistence - reload from disk */
    buckets_topology_manager_cleanup();
    buckets_topology_cache_cleanup();
    buckets_topology_cache_init();
    
    ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    ret = buckets_topology_manager_load();
    cr_assert_eq(ret, BUCKETS_OK, "Reload should succeed");
    
    current = buckets_topology_manager_get();
    cr_assert_not_null(current);
    cr_assert_eq(current->generation, 5, "Generation should be preserved");
    cr_assert_eq(current->pool_count, 1);
    cr_assert_eq(current->pools[0].set_count, 2);
    cr_assert_eq(current->pools[0].sets[0].state, SET_STATE_ACTIVE);
    cr_assert_eq(current->pools[0].sets[1].state, SET_STATE_REMOVED);
}

Test(topology_integration, multi_pool_expansion) {
    /* Simulate cluster expansion with multiple pools */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "multi-pool-test");
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Add 3 pools */
    for (int i = 0; i < 3; i++) {
        ret = buckets_topology_manager_add_pool();
        cr_assert_eq(ret, BUCKETS_OK, "Add pool %d should succeed", i);
    }
    
    buckets_cluster_topology_t *current = buckets_topology_manager_get();
    cr_assert_eq(current->pool_count, 3);
    cr_assert_eq(current->generation, 3);
    
    /* Add sets to each pool */
    buckets_disk_info_t disks[4] = {
        {.endpoint = "http://node:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk2", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk3", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk4", .capacity = 1000000000}
    };
    
    for (int pool_idx = 0; pool_idx < 3; pool_idx++) {
        /* Add 2 sets per pool */
        for (int set = 0; set < 2; set++) {
            ret = buckets_topology_manager_add_set(pool_idx, disks, 4);
            cr_assert_eq(ret, BUCKETS_OK, "Add set to pool %d should succeed", pool_idx);
        }
    }
    
    current = buckets_topology_manager_get();
    cr_assert_eq(current->pool_count, 3);
    cr_assert_eq(current->generation, 9, "Generation should be 9 (3 pools + 6 sets)");
    
    /* Verify all pools have 2 sets */
    for (int i = 0; i < 3; i++) {
        cr_assert_eq(current->pools[i].set_count, 2, "Pool %d should have 2 sets", i);
    }
}

/* ===================================================================
 * Failure Recovery Tests
 * ===================================================================*/

Test(topology_integration, disk_failure_during_change) {
    /* Simulate disk failures during topology changes */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "failure-test");
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Add pool and set */
    ret = buckets_topology_manager_add_pool();
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_disk_info_t disks[4] = {
        {.endpoint = "http://node:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk2", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk3", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk4", .capacity = 1000000000}
    };
    
    ret = buckets_topology_manager_add_set(0, disks, 4);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Simulate disk failure by making 3 disks unavailable */
    rmdir(disk_paths[5]);
    rmdir(disk_paths[6]);
    rmdir(disk_paths[7]);
    
    /* Changes should still succeed with quorum (5/8 disks) */
    ret = buckets_topology_manager_mark_set_draining(0, 0);
    cr_assert_eq(ret, BUCKETS_OK, "Change should succeed with quorum");
    
    /* Verify we can still load with quorum */
    buckets_topology_manager_cleanup();
    buckets_topology_cache_cleanup();
    buckets_topology_cache_init();
    
    ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    ret = buckets_topology_manager_load();
    cr_assert_eq(ret, BUCKETS_OK, "Load should succeed with quorum");
    
    buckets_cluster_topology_t *current = buckets_topology_manager_get();
    cr_assert_not_null(current);
    cr_assert_eq(current->pools[0].sets[0].state, SET_STATE_DRAINING);
}

Test(topology_integration, quorum_loss_blocks_operations) {
    /* Test that operations fail when quorum is lost */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "quorum-loss-test");
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Write initial topology to all disks */
    ret = buckets_topology_manager_add_pool();
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Verify current state */
    buckets_cluster_topology_t *current = buckets_topology_manager_get();
    cr_assert_eq(current->pool_count, 1);
    cr_assert_eq(current->generation, 1);
    
    /* Simulate disk failure by setting paths to NULL (below quorum of 5)
     * This simulates disks becoming unavailable in the manager's configuration */
    char *saved_paths[5];
    for (int i = 3; i < 8; i++) {
        saved_paths[i-3] = disk_paths[i];
        disk_paths[i] = NULL;
    }
    
    /* Reinitialize manager with failed disks */
    buckets_topology_manager_cleanup();
    ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Try to add a set - should fail without write quorum (only 3/8 disks available, need 5) */
    buckets_disk_info_t disks[4] = {
        {.endpoint = "http://node:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk2", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk3", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk4", .capacity = 1000000000}
    };
    
    ret = buckets_topology_manager_add_set(0, disks, 4);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail without quorum (3/8 < 5)");
    
    /* Verify topology unchanged */
    current = buckets_topology_manager_get();
    cr_assert_eq(current->generation, 1, "Generation should be unchanged");
    cr_assert_eq(current->pools[0].set_count, 0, "No set should be added");
    
    /* Restore paths for cleanup */
    for (int i = 3; i < 8; i++) {
        disk_paths[i] = saved_paths[i-3];
    }
}

/* ===================================================================
 * Performance Validation Tests
 * ===================================================================*/

Test(topology_integration, performance_add_pool) {
    /* Validate that add_pool completes in <1 second */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "perf-test");
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    PERF_START();
    ret = buckets_topology_manager_add_pool();
    PERF_END();
    
    cr_assert_eq(ret, BUCKETS_OK);
    
    double elapsed_ms = PERF_MS();
    cr_assert_lt(elapsed_ms, 1000.0, 
                 "Add pool should complete in <1 second (took %.2f ms)", elapsed_ms);
    
    printf("    [PERF] add_pool: %.2f ms\n", elapsed_ms);
}

Test(topology_integration, performance_add_set) {
    /* Validate that add_set completes in <1 second */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "perf-test");
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    ret = buckets_topology_manager_add_pool();
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_disk_info_t disks[4] = {
        {.endpoint = "http://node:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk2", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk3", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk4", .capacity = 1000000000}
    };
    
    PERF_START();
    ret = buckets_topology_manager_add_set(0, disks, 4);
    PERF_END();
    
    cr_assert_eq(ret, BUCKETS_OK);
    
    double elapsed_ms = PERF_MS();
    cr_assert_lt(elapsed_ms, 1000.0,
                 "Add set should complete in <1 second (took %.2f ms)", elapsed_ms);
    
    printf("    [PERF] add_set: %.2f ms\n", elapsed_ms);
}

Test(topology_integration, performance_state_change) {
    /* Validate that state changes complete in <1 second */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "perf-test");
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    ret = buckets_topology_manager_add_pool();
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_disk_info_t disks[4] = {
        {.endpoint = "http://node:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk2", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk3", .capacity = 1000000000},
        {.endpoint = "http://node:9000/disk4", .capacity = 1000000000}
    };
    
    ret = buckets_topology_manager_add_set(0, disks, 4);
    cr_assert_eq(ret, BUCKETS_OK);
    
    PERF_START();
    ret = buckets_topology_manager_mark_set_draining(0, 0);
    PERF_END();
    
    cr_assert_eq(ret, BUCKETS_OK);
    
    double elapsed_ms = PERF_MS();
    cr_assert_lt(elapsed_ms, 1000.0,
                 "State change should complete in <1 second (took %.2f ms)", elapsed_ms);
    
    printf("    [PERF] mark_set_draining: %.2f ms\n", elapsed_ms);
}

Test(topology_integration, performance_load_with_quorum) {
    /* Validate that loading from disk completes quickly */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Create and persist a moderate-sized topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "perf-test");
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Add 2 pools with 2 sets each */
    for (int p = 0; p < 2; p++) {
        ret = buckets_topology_manager_add_pool();
        cr_assert_eq(ret, BUCKETS_OK);
        
        for (int s = 0; s < 2; s++) {
            buckets_disk_info_t disks[4] = {
                {.endpoint = "http://node:9000/disk1", .capacity = 1000000000},
                {.endpoint = "http://node:9000/disk2", .capacity = 1000000000},
                {.endpoint = "http://node:9000/disk3", .capacity = 1000000000},
                {.endpoint = "http://node:9000/disk4", .capacity = 1000000000}
            };
            ret = buckets_topology_manager_add_set(p, disks, 4);
            cr_assert_eq(ret, BUCKETS_OK);
        }
    }
    
    /* Cleanup and reload */
    buckets_topology_manager_cleanup();
    buckets_topology_cache_cleanup();
    buckets_topology_cache_init();
    
    ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    PERF_START();
    ret = buckets_topology_manager_load();
    PERF_END();
    
    cr_assert_eq(ret, BUCKETS_OK);
    
    double elapsed_ms = PERF_MS();
    cr_assert_lt(elapsed_ms, 1000.0,
                 "Load with quorum should complete in <1 second (took %.2f ms)", elapsed_ms);
    
    printf("    [PERF] load_with_quorum: %.2f ms\n", elapsed_ms);
}

/* ===================================================================
 * Stress Tests
 * ===================================================================*/

Test(topology_integration, rapid_consecutive_changes) {
    /* Test rapid consecutive topology changes */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "stress-test");
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Perform 10 rapid operations */
    PERF_START();
    
    for (int i = 0; i < 10; i++) {
        ret = buckets_topology_manager_add_pool();
        cr_assert_eq(ret, BUCKETS_OK, "Operation %d should succeed", i);
    }
    
    PERF_END();
    
    buckets_cluster_topology_t *current = buckets_topology_manager_get();
    cr_assert_eq(current->pool_count, 10);
    cr_assert_eq(current->generation, 10);
    
    double elapsed_ms = PERF_MS();
    printf("    [PERF] 10 consecutive operations: %.2f ms (avg: %.2f ms)\n",
           elapsed_ms, elapsed_ms / 10.0);
}
