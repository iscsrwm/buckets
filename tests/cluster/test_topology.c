/**
 * Criterion Tests for Topology Management
 * 
 * Tests topology.c implementation
 */

#include <criterion/criterion.h>
#include <criterion/redirect.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_io.h"

/* Test fixture: temporary directory for disk operations */
static char test_dir[256];

void setup(void) {
    snprintf(test_dir, sizeof(test_dir), "/tmp/buckets_topology_test_%d", getpid());
    mkdir(test_dir, 0755);
}

void teardown(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    int ret = system(cmd);
    (void)ret;  /* Suppress unused result warning */
}

TestSuite(topology_basic, .init = setup, .fini = teardown);
TestSuite(topology_io, .init = setup, .fini = teardown);
TestSuite(topology_conversion, .init = setup, .fini = teardown);

/* ===== Basic Topology Operations ===== */

Test(topology_basic, create_empty) {
    buckets_cluster_topology_t *topology = buckets_topology_new();
    
    cr_assert_not_null(topology, "topology_new should return non-NULL");
    cr_assert_eq(topology->version, 1, "version should be 1");
    cr_assert_eq(topology->generation, 0, "generation should start at 0");
    cr_assert_eq(topology->vnode_factor, BUCKETS_VNODE_FACTOR, 
                 "vnode_factor should be %d", BUCKETS_VNODE_FACTOR);
    cr_assert_eq(topology->pool_count, 0, "pool_count should be 0");
    cr_assert_null(topology->pools, "pools should be NULL");
    
    buckets_topology_free(topology);
}

Test(topology_basic, free_null) {
    buckets_topology_free(NULL);  /* Should not crash */
}

Test(topology_basic, free_empty) {
    buckets_cluster_topology_t *topology = buckets_topology_new();
    buckets_topology_free(topology);  /* Should not crash */
}

/* ===== Topology from Format ===== */

Test(topology_conversion, from_format_basic) {
    buckets_format_t *format = buckets_format_new(2, 4);
    cr_assert_not_null(format);
    
    buckets_cluster_topology_t *topology = buckets_topology_from_format(format);
    cr_assert_not_null(topology);
    
    /* Check basic fields */
    cr_assert_eq(topology->version, 1);
    cr_assert_eq(topology->generation, 1, "generation should be 1 for new topology");
    cr_assert_str_eq(topology->deployment_id, format->meta.deployment_id);
    cr_assert_eq(topology->vnode_factor, BUCKETS_VNODE_FACTOR);
    
    /* Check pool structure */
    cr_assert_eq(topology->pool_count, 1, "should have 1 pool");
    cr_assert_not_null(topology->pools);
    
    /* Check set structure */
    buckets_pool_topology_t *pool = &topology->pools[0];
    cr_assert_eq(pool->idx, 0);
    cr_assert_eq(pool->set_count, 2);
    cr_assert_not_null(pool->sets);
    
    /* Check sets */
    for (int i = 0; i < 2; i++) {
        buckets_set_topology_t *set = &pool->sets[i];
        cr_assert_eq(set->idx, i);
        cr_assert_eq(set->state, SET_STATE_ACTIVE);
        cr_assert_eq(set->disk_count, 4);
        cr_assert_not_null(set->disks);
        
        /* Check disks have UUIDs */
        for (int j = 0; j < 4; j++) {
            cr_assert_not_null(set->disks[j].uuid[0], "disk UUID should not be empty");
            cr_assert_eq(strlen(set->disks[j].uuid), 36, "UUID should be 36 chars");
        }
    }
    
    buckets_topology_free(topology);
    buckets_format_free(format);
}

Test(topology_conversion, from_format_null) {
    buckets_cluster_topology_t *topology = buckets_topology_from_format(NULL);
    cr_assert_null(topology, "should return NULL for NULL format");
}

Test(topology_conversion, from_format_single_set) {
    buckets_format_t *format = buckets_format_new(1, 8);
    cr_assert_not_null(format);
    
    buckets_cluster_topology_t *topology = buckets_topology_from_format(format);
    cr_assert_not_null(topology);
    
    cr_assert_eq(topology->pool_count, 1);
    cr_assert_eq(topology->pools[0].set_count, 1);
    cr_assert_eq(topology->pools[0].sets[0].disk_count, 8);
    
    buckets_topology_free(topology);
    buckets_format_free(format);
}

Test(topology_conversion, from_format_many_sets) {
    buckets_format_t *format = buckets_format_new(8, 4);
    cr_assert_not_null(format);
    
    buckets_cluster_topology_t *topology = buckets_topology_from_format(format);
    cr_assert_not_null(topology);
    
    cr_assert_eq(topology->pool_count, 1);
    cr_assert_eq(topology->pools[0].set_count, 8);
    
    /* Verify all sets */
    for (int i = 0; i < 8; i++) {
        cr_assert_eq(topology->pools[0].sets[i].idx, i);
        cr_assert_eq(topology->pools[0].sets[i].disk_count, 4);
        cr_assert_eq(topology->pools[0].sets[i].state, SET_STATE_ACTIVE);
    }
    
    buckets_topology_free(topology);
    buckets_format_free(format);
}

/* ===== Topology I/O ===== */

Test(topology_io, save_load_roundtrip) {
    /* Create topology */
    buckets_format_t *format = buckets_format_new(2, 4);
    cr_assert_not_null(format);
    
    buckets_cluster_topology_t *original = buckets_topology_from_format(format);
    cr_assert_not_null(original);
    
    /* Save */
    int ret = buckets_topology_save(test_dir, original);
    cr_assert_eq(ret, BUCKETS_OK, "save should succeed");
    
    /* Verify file exists */
    char path[512];
    snprintf(path, sizeof(path), "%s/.buckets.sys/topology.json", test_dir);
    struct stat st;
    cr_assert_eq(stat(path, &st), 0, "topology.json should exist");
    
    /* Load */
    buckets_cluster_topology_t *loaded = buckets_topology_load(test_dir);
    cr_assert_not_null(loaded, "load should succeed");
    
    /* Compare */
    cr_assert_eq(loaded->version, original->version);
    cr_assert_eq(loaded->generation, original->generation);
    cr_assert_str_eq(loaded->deployment_id, original->deployment_id);
    cr_assert_eq(loaded->vnode_factor, original->vnode_factor);
    cr_assert_eq(loaded->pool_count, original->pool_count);
    
    /* Compare pool structure */
    for (int p = 0; p < original->pool_count; p++) {
        cr_assert_eq(loaded->pools[p].set_count, original->pools[p].set_count);
        
        for (int s = 0; s < original->pools[p].set_count; s++) {
            buckets_set_topology_t *orig_set = &original->pools[p].sets[s];
            buckets_set_topology_t *load_set = &loaded->pools[p].sets[s];
            
            cr_assert_eq(load_set->idx, orig_set->idx);
            cr_assert_eq(load_set->state, orig_set->state);
            cr_assert_eq(load_set->disk_count, orig_set->disk_count);
            
            for (int d = 0; d < orig_set->disk_count; d++) {
                cr_assert_str_eq(load_set->disks[d].uuid, orig_set->disks[d].uuid);
            }
        }
    }
    
    buckets_topology_free(loaded);
    buckets_topology_free(original);
    buckets_format_free(format);
}

Test(topology_io, save_null_path) {
    buckets_cluster_topology_t *topology = buckets_topology_new();
    int ret = buckets_topology_save(NULL, topology);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG);
    buckets_topology_free(topology);
}

Test(topology_io, save_null_topology) {
    int ret = buckets_topology_save(test_dir, NULL);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG);
}

Test(topology_io, load_null_path) {
    buckets_cluster_topology_t *topology = buckets_topology_load(NULL);
    cr_assert_null(topology);
}

Test(topology_io, load_nonexistent) {
    buckets_cluster_topology_t *topology = buckets_topology_load("/nonexistent/path");
    cr_assert_null(topology);
}

Test(topology_io, load_empty_file) {
    /* Create empty topology.json */
    char path[512];
    snprintf(path, sizeof(path), "%s/.buckets.sys", test_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/.buckets.sys/topology.json", test_dir);
    FILE *f = fopen(path, "w");
    fclose(f);
    
    buckets_cluster_topology_t *topology = buckets_topology_load(test_dir);
    cr_assert_null(topology, "should fail on empty file");
}

Test(topology_io, save_creates_directory) {
    buckets_format_t *format = buckets_format_new(1, 4);
    buckets_cluster_topology_t *topology = buckets_topology_from_format(format);
    
    /* Save should create .buckets.sys directory */
    int ret = buckets_topology_save(test_dir, topology);
    cr_assert_eq(ret, BUCKETS_OK);
    
    char path[512];
    snprintf(path, sizeof(path), "%s/.buckets.sys", test_dir);
    struct stat st;
    cr_assert_eq(stat(path, &st), 0, ".buckets.sys should be created");
    cr_assert(S_ISDIR(st.st_mode), ".buckets.sys should be a directory");
    
    buckets_topology_free(topology);
    buckets_format_free(format);
}

/* ===== Generation Number ===== */

Test(topology_basic, generation_starts_at_one) {
    buckets_format_t *format = buckets_format_new(1, 4);
    buckets_cluster_topology_t *topology = buckets_topology_from_format(format);
    
    cr_assert_eq(topology->generation, 1, "generation should start at 1");
    
    buckets_topology_free(topology);
    buckets_format_free(format);
}

Test(topology_basic, generation_preserved_on_save_load) {
    buckets_format_t *format = buckets_format_new(1, 4);
    buckets_cluster_topology_t *topology = buckets_topology_from_format(format);
    
    /* Manually increment generation */
    topology->generation = 42;
    
    /* Save and load */
    buckets_topology_save(test_dir, topology);
    buckets_cluster_topology_t *loaded = buckets_topology_load(test_dir);
    
    cr_assert_eq(loaded->generation, 42, "generation should be preserved");
    
    buckets_topology_free(loaded);
    buckets_topology_free(topology);
    buckets_format_free(format);
}

/* ===== Set State ===== */

Test(topology_basic, default_set_state_is_active) {
    buckets_format_t *format = buckets_format_new(2, 4);
    buckets_cluster_topology_t *topology = buckets_topology_from_format(format);
    
    for (int i = 0; i < topology->pools[0].set_count; i++) {
        cr_assert_eq(topology->pools[0].sets[i].state, SET_STATE_ACTIVE);
    }
    
    buckets_topology_free(topology);
    buckets_format_free(format);
}

Test(topology_basic, set_state_preserved_on_save_load) {
    buckets_format_t *format = buckets_format_new(3, 4);
    buckets_cluster_topology_t *topology = buckets_topology_from_format(format);
    
    /* Change set states */
    topology->pools[0].sets[0].state = SET_STATE_ACTIVE;
    topology->pools[0].sets[1].state = SET_STATE_DRAINING;
    topology->pools[0].sets[2].state = SET_STATE_REMOVED;
    
    /* Save and load */
    buckets_topology_save(test_dir, topology);
    buckets_cluster_topology_t *loaded = buckets_topology_load(test_dir);
    
    cr_assert_eq(loaded->pools[0].sets[0].state, SET_STATE_ACTIVE);
    cr_assert_eq(loaded->pools[0].sets[1].state, SET_STATE_DRAINING);
    cr_assert_eq(loaded->pools[0].sets[2].state, SET_STATE_REMOVED);
    
    buckets_topology_free(loaded);
    buckets_topology_free(topology);
    buckets_format_free(format);
}
