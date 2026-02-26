/**
 * Topology Manager Tests
 * 
 * Tests for high-level topology coordination API:
 * - Manager initialization/cleanup
 * - Topology loading with quorum
 * - Coordinated topology changes
 * - Event callbacks
 * - Cache integration
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_cache.h"

/* Test fixture */
static char test_dir[256];
static char *disk_paths[5];
static int disk_count = 5;

/* Callback tracking */
static int callback_count = 0;
static i64 last_callback_generation = 0;

void topology_change_callback(buckets_cluster_topology_t *topology, void *user_data)
{
    callback_count++;
    last_callback_generation = topology->generation;
    int *custom_data = (int*)user_data;
    if (custom_data) {
        (*custom_data)++;
    }
}

void setup_manager(void) {
    /* Initialize buckets */
    buckets_init();
    
    /* Initialize caches */
    buckets_topology_cache_init();
    
    /* Create unique test directory */
    snprintf(test_dir, sizeof(test_dir), "/tmp/buckets_test_manager_%d_%ld",
             getpid(), (long)time(NULL));
    mkdir(test_dir, 0755);
    
    /* Initialize disk paths */
    for (int i = 0; i < disk_count; i++) {
        disk_paths[i] = buckets_format("%s/disk%d", test_dir, i);
        mkdir(disk_paths[i], 0755);
    }
    
    /* Reset callback tracking */
    callback_count = 0;
    last_callback_generation = 0;
}

void teardown_manager(void) {
    /* Cleanup manager */
    buckets_topology_manager_cleanup();
    
    /* Cleanup cache */
    buckets_topology_cache_cleanup();
    
    /* Clean up disk paths */
    for (int i = 0; i < disk_count; i++) {
        if (disk_paths[i]) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "rm -rf %s", disk_paths[i]);
            if (system(cmd) != 0) {
                /* Ignore cleanup errors */
            }
            buckets_free(disk_paths[i]);
            disk_paths[i] = NULL;
        }
    }
    
    /* Remove test directory */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    if (system(cmd) != 0) {
        /* Ignore cleanup errors */
    }
    
    buckets_cleanup();
}

TestSuite(topology_manager, .init = setup_manager, .fini = teardown_manager);

/* ===================================================================
 * Initialization Tests
 * ===================================================================*/

Test(topology_manager, init_and_cleanup) {
    /* Initialize manager */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK, "Manager init should succeed");
    
    /* Try double init (should fail) */
    ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_neq(ret, BUCKETS_OK, "Double init should fail");
    
    /* Cleanup */
    buckets_topology_manager_cleanup();
    
    /* Double cleanup should be safe */
    buckets_topology_manager_cleanup();
}

Test(topology_manager, init_invalid_args) {
    /* NULL disk paths */
    int ret = buckets_topology_manager_init(NULL, disk_count);
    cr_assert_neq(ret, BUCKETS_OK, "Should reject NULL disk paths");
    
    /* Zero disk count */
    ret = buckets_topology_manager_init(disk_paths, 0);
    cr_assert_neq(ret, BUCKETS_OK, "Should reject zero disk count");
}

/* ===================================================================
 * Load Tests
 * ===================================================================*/

Test(topology_manager, load_with_quorum) {
    /* Initialize manager */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Create and save a topology to all disks */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "test-deployment-id-12345");
    topology->generation = 42;
    
    ret = buckets_topology_add_pool(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Save to all disks */
    ret = buckets_topology_save_quorum(disk_paths, disk_count, topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_topology_free(topology);
    
    /* Load with manager */
    ret = buckets_topology_manager_load();
    cr_assert_eq(ret, BUCKETS_OK, "Manager load should succeed");
    
    /* Get topology from manager */
    buckets_cluster_topology_t *loaded = buckets_topology_manager_get();
    cr_assert_not_null(loaded, "Should get topology from manager");
    cr_assert_eq(loaded->generation, 43, "Generation should be 43 (after add_pool)");
    cr_assert_eq(loaded->pool_count, 1, "Should have 1 pool");
}

Test(topology_manager, load_with_quorum_failure) {
    /* Initialize manager */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Don't save any topology files */
    
    /* Load should fail (no quorum) */
    ret = buckets_topology_manager_load();
    cr_assert_neq(ret, BUCKETS_OK, "Load should fail without quorum");
}

/* ===================================================================
 * Coordinated Change Tests
 * ===================================================================*/

Test(topology_manager, add_pool_coordinated) {
    /* Initialize manager */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Create initial topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "test-deployment-id");
    topology->generation = 0;
    
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Add pool through manager */
    ret = buckets_topology_manager_add_pool();
    cr_assert_eq(ret, BUCKETS_OK, "Add pool should succeed");
    
    /* Verify topology was updated */
    buckets_cluster_topology_t *updated = buckets_topology_manager_get();
    cr_assert_not_null(updated);
    cr_assert_eq(updated->pool_count, 1, "Should have 1 pool");
    cr_assert_eq(updated->generation, 1, "Generation should increment");
    
    /* Verify persistence on disk */
    buckets_cluster_topology_t *reloaded = 
        buckets_topology_load_quorum(disk_paths, disk_count);
    cr_assert_not_null(reloaded, "Should reload from disk");
    cr_assert_eq(reloaded->pool_count, 1);
    cr_assert_eq(reloaded->generation, 1);
    
    buckets_topology_free(reloaded);
}

Test(topology_manager, add_set_coordinated) {
    /* Initialize manager */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Create initial topology with one pool */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "test-deployment-id");
    topology->generation = 0;
    
    ret = buckets_topology_add_pool(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Define disks for set */
    buckets_disk_info_t disks[4] = {
        {.endpoint = "http://node1:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node1:9000/disk2", .capacity = 1000000000},
        {.endpoint = "http://node2:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node2:9000/disk2", .capacity = 1000000000}
    };
    
    /* Add set through manager */
    ret = buckets_topology_manager_add_set(0, disks, 4);
    cr_assert_eq(ret, BUCKETS_OK, "Add set should succeed");
    
    /* Verify topology was updated */
    buckets_cluster_topology_t *updated = buckets_topology_manager_get();
    cr_assert_not_null(updated);
    cr_assert_eq(updated->pool_count, 1);
    cr_assert_eq(updated->pools[0].set_count, 1, "Pool should have 1 set");
    cr_assert_eq(updated->generation, 2, "Generation should be 2");
}

Test(topology_manager, mark_set_draining_coordinated) {
    /* Initialize manager and topology with pool and set */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "test-deployment-id");
    
    ret = buckets_topology_add_pool(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_disk_info_t disks[4] = {
        {.endpoint = "http://node1:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node1:9000/disk2", .capacity = 1000000000},
        {.endpoint = "http://node2:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node2:9000/disk2", .capacity = 1000000000}
    };
    
    ret = buckets_topology_add_set(topology, 0, disks, 4);
    cr_assert_eq(ret, BUCKETS_OK);
    
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Mark set draining through manager */
    ret = buckets_topology_manager_mark_set_draining(0, 0);
    cr_assert_eq(ret, BUCKETS_OK, "Mark draining should succeed");
    
    /* Verify state changed */
    buckets_cluster_topology_t *updated = buckets_topology_manager_get();
    cr_assert_not_null(updated);
    cr_assert_eq(updated->pools[0].sets[0].state, SET_STATE_DRAINING);
    cr_assert_eq(updated->generation, 3, "Generation should increment");
}

/* ===================================================================
 * Callback Tests
 * ===================================================================*/

Test(topology_manager, callback_on_change) {
    /* Initialize manager */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Register callback */
    ret = buckets_topology_manager_set_callback(topology_change_callback, NULL);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Create initial topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "test-deployment-id");
    topology->generation = 0;
    
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Make a change */
    callback_count = 0;
    ret = buckets_topology_manager_add_pool();
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Verify callback was invoked */
    cr_assert_eq(callback_count, 1, "Callback should be invoked once");
    cr_assert_eq(last_callback_generation, 1, "Callback should see generation 1");
}

Test(topology_manager, callback_with_user_data) {
    /* Initialize manager */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* User data */
    int custom_counter = 0;
    
    /* Register callback with user data */
    ret = buckets_topology_manager_set_callback(topology_change_callback, &custom_counter);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Create initial topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "test-deployment-id");
    
    ret = buckets_topology_cache_set(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Make changes */
    buckets_topology_manager_add_pool();
    buckets_topology_manager_add_pool();
    
    /* Verify user data was updated */
    cr_assert_eq(custom_counter, 2, "User data should be incremented twice");
}

/* ===================================================================
 * Error Handling Tests
 * ===================================================================*/

Test(topology_manager, operations_without_init) {
    /* Don't initialize manager */
    
    /* All operations should fail gracefully */
    int ret = buckets_topology_manager_load();
    cr_assert_neq(ret, BUCKETS_OK);
    
    ret = buckets_topology_manager_add_pool();
    cr_assert_neq(ret, BUCKETS_OK);
    
    buckets_disk_info_t disks[4] = {
        {.endpoint = "", .capacity = 0},
        {.endpoint = "", .capacity = 0},
        {.endpoint = "", .capacity = 0},
        {.endpoint = "", .capacity = 0}
    };
    ret = buckets_topology_manager_add_set(0, disks, 4);
    cr_assert_neq(ret, BUCKETS_OK);
    
    ret = buckets_topology_manager_mark_set_draining(0, 0);
    cr_assert_neq(ret, BUCKETS_OK);
    
    ret = buckets_topology_manager_mark_set_removed(0, 0);
    cr_assert_neq(ret, BUCKETS_OK);
    
    buckets_cluster_topology_t *topo = buckets_topology_manager_get();
    cr_assert_null(topo);
}

Test(topology_manager, operations_without_topology) {
    /* Initialize manager but don't load topology */
    int ret = buckets_topology_manager_init(disk_paths, disk_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Operations should fail without topology in cache */
    ret = buckets_topology_manager_add_pool();
    cr_assert_neq(ret, BUCKETS_OK, "Should fail without topology");
}
