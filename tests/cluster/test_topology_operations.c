/**
 * Topology Operations Tests
 * 
 * Tests for dynamic topology modification operations (add pool, add set, state changes)
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <string.h>

#include "buckets.h"
#include "buckets_cluster.h"

/* Test: Add pool to topology */
Test(topology_ops, add_pool)
{
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    cr_assert_eq(topology->pool_count, 0);
    cr_assert_eq(topology->generation, 0);
    
    /* Add first pool */
    int result = buckets_topology_add_pool(topology);
    cr_assert_eq(result, BUCKETS_OK);
    cr_assert_eq(topology->pool_count, 1);
    cr_assert_eq(topology->generation, 1, "Generation should increment");
    cr_assert_not_null(topology->pools);
    cr_assert_eq(topology->pools[0].idx, 0);
    cr_assert_eq(topology->pools[0].set_count, 0);
    
    /* Add second pool */
    result = buckets_topology_add_pool(topology);
    cr_assert_eq(result, BUCKETS_OK);
    cr_assert_eq(topology->pool_count, 2);
    cr_assert_eq(topology->generation, 2);
    cr_assert_eq(topology->pools[1].idx, 1);
    
    buckets_topology_free(topology);
}

/* Test: Add set to pool */
Test(topology_ops, add_set_to_pool)
{
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    /* Add a pool first */
    int result = buckets_topology_add_pool(topology);
    cr_assert_eq(result, BUCKETS_OK);
    
    /* Create disk info */
    buckets_disk_info_t disks[4];
    for (int i = 0; i < 4; i++) {
        snprintf(disks[i].endpoint, sizeof(disks[i].endpoint), "http://node1:9000/disk%d", i);
        snprintf(disks[i].uuid, sizeof(disks[i].uuid), "disk-uuid-%d", i);
        disks[i].capacity = 1024ULL * 1024 * 1024 * 1024;  /* 1 TB */
    }
    
    /* Add set to pool 0 */
    result = buckets_topology_add_set(topology, 0, disks, 4);
    cr_assert_eq(result, BUCKETS_OK);
    cr_assert_eq(topology->pools[0].set_count, 1);
    cr_assert_eq(topology->generation, 2);  /* 1 for add_pool, 1 for add_set */
    
    /* Verify set details */
    buckets_set_topology_t *set = &topology->pools[0].sets[0];
    cr_assert_eq(set->idx, 0);
    cr_assert_eq(set->state, SET_STATE_ACTIVE);
    cr_assert_eq(set->disk_count, 4);
    cr_assert_not_null(set->disks);
    
    /* Verify disk details */
    for (int i = 0; i < 4; i++) {
        char expected_endpoint[256];
        snprintf(expected_endpoint, sizeof(expected_endpoint), "http://node1:9000/disk%d", i);
        cr_assert_str_eq(set->disks[i].endpoint, expected_endpoint);
    }
    
    buckets_topology_free(topology);
}

/* Test: Add multiple sets to pool */
Test(topology_ops, add_multiple_sets)
{
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    /* Add a pool */
    buckets_topology_add_pool(topology);
    
    /* Add 3 sets */
    for (int s = 0; s < 3; s++) {
        buckets_disk_info_t disks[4];
        for (int i = 0; i < 4; i++) {
            snprintf(disks[i].endpoint, sizeof(disks[i].endpoint),
                    "http://node1:9000/set%d-disk%d", s, i);
            snprintf(disks[i].uuid, sizeof(disks[i].uuid), "uuid-s%d-d%d", s, i);
            disks[i].capacity = 1024ULL * 1024 * 1024 * 1024;
        }
        
        int result = buckets_topology_add_set(topology, 0, disks, 4);
        cr_assert_eq(result, BUCKETS_OK);
    }
    
    cr_assert_eq(topology->pools[0].set_count, 3);
    cr_assert_eq(topology->generation, 4);  /* 1 pool + 3 sets */
    
    /* Verify set indices */
    for (int i = 0; i < 3; i++) {
        cr_assert_eq(topology->pools[0].sets[i].idx, i);
        cr_assert_eq(topology->pools[0].sets[i].state, SET_STATE_ACTIVE);
    }
    
    buckets_topology_free(topology);
}

/* Test: Mark set as draining */
Test(topology_ops, mark_set_draining)
{
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    /* Setup: pool with one set */
    buckets_topology_add_pool(topology);
    
    buckets_disk_info_t disks[4];
    for (int i = 0; i < 4; i++) {
        snprintf(disks[i].endpoint, sizeof(disks[i].endpoint), "http://node1:9000/disk%d", i);
        snprintf(disks[i].uuid, sizeof(disks[i].uuid), "disk-uuid-%d", i);
        disks[i].capacity = 1024ULL * 1024 * 1024 * 1024;
    }
    buckets_topology_add_set(topology, 0, disks, 4);
    
    i64 gen_before = topology->generation;
    cr_assert_eq(topology->pools[0].sets[0].state, SET_STATE_ACTIVE);
    
    /* Mark as draining */
    int result = buckets_topology_mark_set_draining(topology, 0, 0);
    cr_assert_eq(result, BUCKETS_OK);
    cr_assert_eq(topology->pools[0].sets[0].state, SET_STATE_DRAINING);
    cr_assert_eq(topology->generation, gen_before + 1, "Generation should increment");
    
    buckets_topology_free(topology);
}

/* Test: Mark set as removed */
Test(topology_ops, mark_set_removed)
{
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    /* Setup */
    buckets_topology_add_pool(topology);
    
    buckets_disk_info_t disks[4];
    for (int i = 0; i < 4; i++) {
        snprintf(disks[i].endpoint, sizeof(disks[i].endpoint), "http://node1:9000/disk%d", i);
        snprintf(disks[i].uuid, sizeof(disks[i].uuid), "disk-uuid-%d", i);
        disks[i].capacity = 1024ULL * 1024 * 1024 * 1024;
    }
    buckets_topology_add_set(topology, 0, disks, 4);
    
    /* Mark as removed */
    int result = buckets_topology_mark_set_removed(topology, 0, 0);
    cr_assert_eq(result, BUCKETS_OK);
    cr_assert_eq(topology->pools[0].sets[0].state, SET_STATE_REMOVED);
    
    buckets_topology_free(topology);
}

/* Test: State transition workflow */
Test(topology_ops, state_transition_workflow)
{
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    /* Setup */
    buckets_topology_add_pool(topology);
    
    buckets_disk_info_t disks[4];
    for (int i = 0; i < 4; i++) {
        snprintf(disks[i].endpoint, sizeof(disks[i].endpoint), "http://node1:9000/disk%d", i);
        snprintf(disks[i].uuid, sizeof(disks[i].uuid), "disk-uuid-%d", i);
        disks[i].capacity = 1024ULL * 1024 * 1024 * 1024;
    }
    buckets_topology_add_set(topology, 0, disks, 4);
    
    i64 gen_start = topology->generation;
    
    /* ACTIVE -> DRAINING */
    cr_assert_eq(topology->pools[0].sets[0].state, SET_STATE_ACTIVE);
    buckets_topology_mark_set_draining(topology, 0, 0);
    cr_assert_eq(topology->pools[0].sets[0].state, SET_STATE_DRAINING);
    cr_assert_eq(topology->generation, gen_start + 1);
    
    /* DRAINING -> REMOVED */
    buckets_topology_mark_set_removed(topology, 0, 0);
    cr_assert_eq(topology->pools[0].sets[0].state, SET_STATE_REMOVED);
    cr_assert_eq(topology->generation, gen_start + 2);
    
    buckets_topology_free(topology);
}

/* Test: Invalid operations */
Test(topology_ops, invalid_operations)
{
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    /* Try to add set to non-existent pool */
    buckets_disk_info_t disks[4];
    for (int i = 0; i < 4; i++) {
        snprintf(disks[i].endpoint, sizeof(disks[i].endpoint), "http://node1:9000/disk%d", i);
        snprintf(disks[i].uuid, sizeof(disks[i].uuid), "disk-uuid-%d", i);
        disks[i].capacity = 1024ULL * 1024 * 1024 * 1024;
    }
    
    int result = buckets_topology_add_set(topology, 0, disks, 4);
    cr_assert_eq(result, BUCKETS_ERR_INVALID_ARG, "Should fail: pool doesn't exist");
    
    /* Add a pool */
    buckets_topology_add_pool(topology);
    
    /* Try to mark non-existent set */
    result = buckets_topology_mark_set_draining(topology, 0, 0);
    cr_assert_eq(result, BUCKETS_ERR_INVALID_ARG, "Should fail: set doesn't exist");
    
    /* NULL topology */
    result = buckets_topology_add_pool(NULL);
    cr_assert_eq(result, BUCKETS_ERR_INVALID_ARG);
    
    buckets_topology_free(topology);
}

/* Test: Generation tracking */
Test(topology_ops, generation_tracking)
{
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    cr_assert_eq(topology->generation, 0);
    
    /* Each operation should increment generation */
    buckets_topology_add_pool(topology);
    cr_assert_eq(topology->generation, 1);
    
    buckets_disk_info_t disks[4];
    for (int i = 0; i < 4; i++) {
        snprintf(disks[i].endpoint, sizeof(disks[i].endpoint), "http://node1:9000/disk%d", i);
        snprintf(disks[i].uuid, sizeof(disks[i].uuid), "disk-uuid-%d", i);
        disks[i].capacity = 1024ULL * 1024 * 1024 * 1024;
    }
    
    buckets_topology_add_set(topology, 0, disks, 4);
    cr_assert_eq(topology->generation, 2);
    
    buckets_topology_mark_set_draining(topology, 0, 0);
    cr_assert_eq(topology->generation, 3);
    
    buckets_topology_mark_set_removed(topology, 0, 0);
    cr_assert_eq(topology->generation, 4);
    
    buckets_topology_free(topology);
}
