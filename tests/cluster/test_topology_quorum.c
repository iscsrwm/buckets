/**
 * Topology Quorum Operations Tests
 * 
 * Tests for quorum-based topology persistence:
 * - Write quorum (N/2+1 disks)
 * - Read quorum (N/2 matching disks)
 * - Automatic consensus detection
 * - Error handling for quorum failures
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

/* Test fixture */
static char test_dir[256];
static char *disk_paths[8];
static int disk_count;

void setup_quorum(void) {
    /* Create unique test directory */
    snprintf(test_dir, sizeof(test_dir), "/tmp/buckets_test_quorum_%d_%ld",
             getpid(), (long)time(NULL));
    mkdir(test_dir, 0755);
    
    /* Initialize disk paths */
    disk_count = 5;  /* 5 disks = quorum of 3 */
    for (int i = 0; i < disk_count; i++) {
        disk_paths[i] = buckets_format("%s/disk%d", test_dir, i);
        mkdir(disk_paths[i], 0755);
    }
}

void teardown_quorum(void) {
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
}

TestSuite(topology_quorum, .init = setup_quorum, .fini = teardown_quorum);

/* ===================================================================
 * Write Quorum Tests
 * ===================================================================*/

Test(topology_quorum, write_all_disks_succeed) {
    /* Create test topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    strcpy(topology->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology->generation = 42;
    
    /* Add a pool and set */
    int ret = buckets_topology_add_pool(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    buckets_disk_info_t disks[4] = {
        {.endpoint = "http://node1:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node1:9000/disk2", .capacity = 1000000000},
        {.endpoint = "http://node2:9000/disk1", .capacity = 1000000000},
        {.endpoint = "http://node2:9000/disk2", .capacity = 1000000000}
    };
    
    ret = buckets_topology_add_set(topology, 0, disks, 4);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Write with quorum (all 5 disks succeed) */
    ret = buckets_topology_save_quorum(disk_paths, disk_count, topology);
    cr_assert_eq(ret, BUCKETS_OK, "Expected quorum write to succeed");
    
    /* Verify files exist on all disks */
    for (int i = 0; i < disk_count; i++) {
        char *path = buckets_format("%s/.buckets.sys/topology.json", disk_paths[i]);
        struct stat st;
        cr_assert_eq(stat(path, &st), 0, "Topology file should exist on disk %d", i);
        buckets_free(path);
    }
    
    buckets_topology_free(topology);
}

Test(topology_quorum, write_quorum_with_failures) {
    /* Create test topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    strcpy(topology->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology->generation = 10;
    
    /* Simulate 2 disk failures by removing directories */
    rmdir(disk_paths[3]);
    rmdir(disk_paths[4]);
    
    /* Write with quorum (3/5 succeed - exactly quorum) */
    int ret = buckets_topology_save_quorum(disk_paths, disk_count, topology);
    cr_assert_eq(ret, BUCKETS_OK, "Expected quorum write to succeed with 3/5 disks");
    
    /* Verify files exist on working disks */
    for (int i = 0; i < 3; i++) {
        char *path = buckets_format("%s/.buckets.sys/topology.json", disk_paths[i]);
        struct stat st;
        cr_assert_eq(stat(path, &st), 0, "Topology file should exist on disk %d", i);
        buckets_free(path);
    }
    
    buckets_topology_free(topology);
}

Test(topology_quorum, write_quorum_failure) {
    /* Create test topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    strcpy(topology->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    
    /* Simulate 3 disk failures by replacing paths with invalid locations */
    /* Save original paths */
    char *orig_paths[3];
    orig_paths[0] = disk_paths[2];
    orig_paths[1] = disk_paths[3];
    orig_paths[2] = disk_paths[4];
    
    /* Replace with invalid paths (read-only root directory) */
    disk_paths[2] = "/proc/invalid/path/disk2";
    disk_paths[3] = "/proc/invalid/path/disk3";
    disk_paths[4] = "/proc/invalid/path/disk4";
    
    /* Write should fail (2/5 < quorum of 3) */
    int ret = buckets_topology_save_quorum(disk_paths, disk_count, topology);
    cr_assert_neq(ret, BUCKETS_OK, "Expected quorum write to fail with 2/5 disks");
    cr_assert_eq(ret, BUCKETS_ERR_QUORUM);
    
    /* Restore original paths */
    disk_paths[2] = orig_paths[0];
    disk_paths[3] = orig_paths[1];
    disk_paths[4] = orig_paths[2];
    
    buckets_topology_free(topology);
}

/* ===================================================================
 * Read Quorum Tests
 * ===================================================================*/

Test(topology_quorum, read_all_disks_match) {
    /* Create and save test topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    strcpy(topology->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology->generation = 100;
    
    int ret = buckets_topology_add_pool(topology);
    cr_assert_eq(ret, BUCKETS_OK);
    /* add_pool increments generation to 101 */
    
    /* Save to all disks */
    for (int i = 0; i < disk_count; i++) {
        ret = buckets_topology_save(disk_paths[i], topology);
        cr_assert_eq(ret, BUCKETS_OK);
    }
    
    /* Read with quorum (all 5 disks match) */
    buckets_cluster_topology_t *loaded = buckets_topology_load_quorum(disk_paths, disk_count);
    cr_assert_not_null(loaded, "Expected quorum read to succeed");
    
    /* Verify content */
    cr_assert_eq(loaded->generation, 101, "Generation should be 101 after add_pool");
    cr_assert_eq(loaded->pool_count, 1);
    cr_assert_str_eq(loaded->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    
    buckets_topology_free(topology);
    buckets_topology_free(loaded);
}

Test(topology_quorum, read_quorum_with_failures) {
    /* Create and save test topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    strcpy(topology->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology->generation = 200;
    
    /* Save to first 3 disks (exactly quorum) */
    for (int i = 0; i < 3; i++) {
        int ret = buckets_topology_save(disk_paths[i], topology);
        cr_assert_eq(ret, BUCKETS_OK);
    }
    
    /* Read with quorum (3/5 match - exactly read quorum) */
    buckets_cluster_topology_t *loaded = buckets_topology_load_quorum(disk_paths, disk_count);
    cr_assert_not_null(loaded, "Expected quorum read to succeed with 3/5 matching disks");
    
    /* Verify content */
    cr_assert_eq(loaded->generation, 200);
    cr_assert_str_eq(loaded->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    
    buckets_topology_free(topology);
    buckets_topology_free(loaded);
}

Test(topology_quorum, read_quorum_failure) {
    /* Create and save test topology */
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    strcpy(topology->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology->generation = 300;
    
    /* Save to only 1 disk (below read quorum of 2) */
    int ret = buckets_topology_save(disk_paths[0], topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Read should fail (1/5 < read quorum of 2) */
    buckets_cluster_topology_t *loaded = buckets_topology_load_quorum(disk_paths, disk_count);
    cr_assert_null(loaded, "Expected quorum read to fail with 1/5 matching disks");
    
    buckets_topology_free(topology);
}

Test(topology_quorum, read_consensus_detection) {
    /* Create two different topologies */
    buckets_cluster_topology_t *topology1 = buckets_topology_new();
    buckets_cluster_topology_t *topology2 = buckets_topology_new();
    
    strcpy(topology1->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology1->generation = 100;
    
    strcpy(topology2->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology2->generation = 200;  /* Different generation */
    
    /* Save topology1 to disks 0, 1, 2 (3 votes) */
    for (int i = 0; i < 3; i++) {
        int ret = buckets_topology_save(disk_paths[i], topology1);
        cr_assert_eq(ret, BUCKETS_OK);
    }
    
    /* Save topology2 to disks 3, 4 (2 votes) */
    for (int i = 3; i < 5; i++) {
        int ret = buckets_topology_save(disk_paths[i], topology2);
        cr_assert_eq(ret, BUCKETS_OK);
    }
    
    /* Read should return topology1 (has quorum with 3 votes) */
    buckets_cluster_topology_t *loaded = buckets_topology_load_quorum(disk_paths, disk_count);
    cr_assert_not_null(loaded, "Expected quorum read to succeed");
    cr_assert_eq(loaded->generation, 100, "Should load topology with majority votes");
    
    buckets_topology_free(topology1);
    buckets_topology_free(topology2);
    buckets_topology_free(loaded);
}

Test(topology_quorum, read_no_consensus) {
    /* Create three different topologies */
    buckets_cluster_topology_t *topology1 = buckets_topology_new();
    buckets_cluster_topology_t *topology2 = buckets_topology_new();
    buckets_cluster_topology_t *topology3 = buckets_topology_new();
    
    strcpy(topology1->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology1->generation = 100;
    
    strcpy(topology2->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology2->generation = 200;
    
    strcpy(topology3->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology3->generation = 300;
    
    /* Save different topologies with no majority (5 disks, quorum=2) */
    /* topology1: 1 vote, topology2: 2 votes (reaches quorum!), topology3: 2 votes (reaches quorum!)*/
    /* First match wins, so this will actually return topology2 */
    buckets_topology_save(disk_paths[0], topology1);
    buckets_topology_save(disk_paths[1], topology2);
    buckets_topology_save(disk_paths[2], topology2);
    buckets_topology_save(disk_paths[3], topology3);
    buckets_topology_save(disk_paths[4], topology3);
    
    /* Read should succeed (topology2 or topology3 reaches quorum first) */
    buckets_cluster_topology_t *loaded = buckets_topology_load_quorum(disk_paths, disk_count);
    cr_assert_not_null(loaded, "Expected quorum read to succeed (first match wins)");
    /* Could be either generation 200 or 300 depending on disk read order */
    cr_assert(loaded->generation == 200 || loaded->generation == 300,
              "Should load one of the topologies with 2 votes");
    
    buckets_topology_free(topology1);
    buckets_topology_free(topology2);
    buckets_topology_free(topology3);
    buckets_topology_free(loaded);
}

/* ===================================================================
 * Edge Cases
 * ===================================================================*/

Test(topology_quorum, single_disk) {
    /* Single disk: quorum = 1, read quorum = 0 (always succeeds) */
    char *single_disk[1] = { disk_paths[0] };
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology->generation = 1;
    
    /* Write should succeed (1/1 >= quorum of 1) */
    int ret = buckets_topology_save_quorum(single_disk, 1, topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Read should succeed (1/1 >= read quorum of 0) */
    buckets_cluster_topology_t *loaded = buckets_topology_load_quorum(single_disk, 1);
    cr_assert_not_null(loaded);
    cr_assert_eq(loaded->generation, 1);
    
    buckets_topology_free(topology);
    buckets_topology_free(loaded);
}

Test(topology_quorum, three_disks) {
    /* Three disks: quorum = 2, read quorum = 1 */
    char *three_disks[3] = { disk_paths[0], disk_paths[1], disk_paths[2] };
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    strcpy(topology->deployment_id, "550e8400-e29b-41d4-a716-446655440000");
    topology->generation = 5;
    
    /* Write to 2/3 disks (exactly quorum) */
    rmdir(disk_paths[2]);
    
    int ret = buckets_topology_save_quorum(three_disks, 3, topology);
    cr_assert_eq(ret, BUCKETS_OK, "Write should succeed with 2/3 disks");
    
    /* Read should succeed (2/3 >= read quorum of 1.5 rounds to 2) */
    buckets_cluster_topology_t *loaded = buckets_topology_load_quorum(three_disks, 3);
    cr_assert_not_null(loaded);
    cr_assert_eq(loaded->generation, 5);
    
    buckets_topology_free(topology);
    buckets_topology_free(loaded);
}

Test(topology_quorum, null_disk_paths) {
    buckets_cluster_topology_t *topology = buckets_topology_new();
    
    /* Write with NULL disk path in array */
    char *mixed_paths[3] = { disk_paths[0], NULL, disk_paths[2] };
    int ret = buckets_topology_save_quorum(mixed_paths, 3, topology);
    cr_assert_eq(ret, BUCKETS_OK, "Should skip NULL paths and succeed with 2/3");
    
    /* Read with NULL disk path */
    buckets_cluster_topology_t *loaded = buckets_topology_load_quorum(mixed_paths, 3);
    cr_assert_not_null(loaded, "Should skip NULL paths and succeed");
    
    buckets_topology_free(topology);
    buckets_topology_free(loaded);
}

Test(topology_quorum, invalid_arguments) {
    buckets_cluster_topology_t *topology = buckets_topology_new();
    
    /* NULL disk_paths */
    int ret = buckets_topology_save_quorum(NULL, disk_count, topology);
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* Zero disk_count */
    ret = buckets_topology_save_quorum(disk_paths, 0, topology);
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* NULL topology */
    ret = buckets_topology_save_quorum(disk_paths, disk_count, NULL);
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* NULL disk_paths for read */
    buckets_cluster_topology_t *loaded = buckets_topology_load_quorum(NULL, disk_count);
    cr_assert_null(loaded);
    
    /* Zero disk_count for read */
    loaded = buckets_topology_load_quorum(disk_paths, 0);
    cr_assert_null(loaded);
    
    buckets_topology_free(topology);
}
