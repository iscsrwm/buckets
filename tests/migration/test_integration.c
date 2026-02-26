/**
 * Migration Integration Tests (Week 30)
 * 
 * Tests end-to-end migration workflows including:
 * - Periodic checkpointing
 * - Crash recovery (checkpoint save/load/resume)
 * - Complete migration lifecycle
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_migration.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

static char *g_disk_paths[4];
static buckets_cluster_topology_t *g_old_topology = NULL;
static buckets_cluster_topology_t *g_new_topology = NULL;
static char g_test_dir[512];

void setup(void)
{
    /* Initialize Buckets */
    buckets_init();
    
    /* Create unique test directory */
    snprintf(g_test_dir, sizeof(g_test_dir), 
             "/tmp/buckets-test-integration-%d-%ld", 
             getpid(), (long)time(NULL));
    mkdir(g_test_dir, 0755);
    
    /* Create disk paths */
    for (int i = 0; i < 4; i++) {
        g_disk_paths[i] = buckets_malloc(256);
        snprintf(g_disk_paths[i], 256, "%s/disk%d", g_test_dir, i);
        mkdir(g_disk_paths[i], 0755);
    }
    
    /* Create old topology (generation 1) */
    g_old_topology = buckets_calloc(1, sizeof(buckets_cluster_topology_t));
    g_old_topology->generation = 1;
    g_old_topology->pool_count = 1;
    g_old_topology->pools = buckets_calloc(1, sizeof(buckets_pool_topology_t));
    g_old_topology->pools[0].set_count = 1;
    g_old_topology->pools[0].sets = buckets_calloc(1, sizeof(buckets_set_topology_t));
    g_old_topology->pools[0].sets[0].state = SET_STATE_ACTIVE;
    g_old_topology->pools[0].sets[0].disk_count = 4;
    
    /* Create new topology (generation 2) */
    g_new_topology = buckets_calloc(1, sizeof(buckets_cluster_topology_t));
    g_new_topology->generation = 2;
    g_new_topology->pool_count = 1;
    g_new_topology->pools = buckets_calloc(1, sizeof(buckets_pool_topology_t));
    g_new_topology->pools[0].set_count = 1;
    g_new_topology->pools[0].sets = buckets_calloc(1, sizeof(buckets_set_topology_t));
    g_new_topology->pools[0].sets[0].state = SET_STATE_ACTIVE;
    g_new_topology->pools[0].sets[0].disk_count = 4;
}

void teardown(void)
{
    /* Cleanup topologies */
    if (g_old_topology) {
        if (g_old_topology->pools) {
            if (g_old_topology->pools[0].sets) {
                buckets_free(g_old_topology->pools[0].sets);
            }
            buckets_free(g_old_topology->pools);
        }
        buckets_free(g_old_topology);
        g_old_topology = NULL;
    }
    
    if (g_new_topology) {
        if (g_new_topology->pools) {
            if (g_new_topology->pools[0].sets) {
                buckets_free(g_new_topology->pools[0].sets);
            }
            buckets_free(g_new_topology->pools);
        }
        buckets_free(g_new_topology);
        g_new_topology = NULL;
    }
    
    /* Cleanup disk paths */
    for (int i = 0; i < 4; i++) {
        if (g_disk_paths[i]) {
            buckets_free(g_disk_paths[i]);
            g_disk_paths[i] = NULL;
        }
    }
    
    /* Cleanup Buckets */
    buckets_cleanup();
}

TestSuite(migration_integration, .init = setup, .fini = teardown);

/* ===================================================================
 * Integration Tests
 * ===================================================================*/

/**
 * Test 1: Checkpoint initialization
 */
Test(migration_integration, checkpoint_initialization)
{
    buckets_migration_job_t *job = buckets_migration_job_create(
        1, 2, g_old_topology, g_new_topology, g_disk_paths, 4);
    
    cr_assert_not_null(job, "Job creation failed");
    cr_assert_eq(job->last_checkpoint_time, 0, "Checkpoint time should be 0 initially");
    cr_assert_eq(job->last_checkpoint_objects, 0, "Checkpoint objects should be 0");
    cr_assert_not_null(job->checkpoint_path, "Checkpoint path should be set");
    
    /* Verify checkpoint path format */
    cr_assert(strstr(job->checkpoint_path, "migration-gen-1-to-2") != NULL,
              "Checkpoint path should contain job ID");
    
    buckets_migration_job_cleanup(job);
}

/**
 * Test 2: Checkpoint save/load roundtrip
 */
Test(migration_integration, checkpoint_save_load_roundtrip)
{
    /* Create job with some progress */
    buckets_migration_job_t *job = buckets_migration_job_create(
        1, 2, g_old_topology, g_new_topology, g_disk_paths, 4);
    
    job->total_objects = 10000;
    job->migrated_objects = 5000;
    job->failed_objects = 10;
    job->bytes_total = 1024 * 1024 * 100;  /* 100 MB */
    job->bytes_migrated = 1024 * 1024 * 50;  /* 50 MB */
    job->state = BUCKETS_MIGRATION_STATE_MIGRATING;
    job->start_time = time(NULL) - 3600;  /* Started 1 hour ago */
    
    /* Save checkpoint */
    char checkpoint_path[512];
    snprintf(checkpoint_path, sizeof(checkpoint_path), 
             "%s/test-checkpoint.json", g_test_dir);
    
    int ret = buckets_migration_job_save(job, checkpoint_path);
    cr_assert_eq(ret, BUCKETS_OK, "Checkpoint save failed");
    
    /* Load checkpoint */
    buckets_migration_job_t *loaded = buckets_migration_job_load(checkpoint_path);
    cr_assert_not_null(loaded, "Checkpoint load failed");
    
    /* Verify all fields */
    cr_assert_eq(loaded->source_generation, 1, "Source generation mismatch");
    cr_assert_eq(loaded->target_generation, 2, "Target generation mismatch");
    cr_assert_eq(loaded->total_objects, 10000, "Total objects mismatch");
    cr_assert_eq(loaded->migrated_objects, 5000, "Migrated objects mismatch");
    cr_assert_eq(loaded->failed_objects, 10, "Failed objects mismatch");
    cr_assert_eq(loaded->bytes_total, 1024 * 1024 * 100, "Bytes total mismatch");
    cr_assert_eq(loaded->bytes_migrated, 1024 * 1024 * 50, "Bytes migrated mismatch");
    cr_assert_eq(loaded->state, BUCKETS_MIGRATION_STATE_MIGRATING, "State mismatch");
    
    buckets_migration_job_cleanup(job);
    buckets_migration_job_cleanup(loaded);
}

/**
 * Test 3: Resume from checkpoint
 */
Test(migration_integration, resume_from_checkpoint)
{
    /* Create and save a checkpoint */
    buckets_migration_job_t *job = buckets_migration_job_create(
        1, 2, g_old_topology, g_new_topology, g_disk_paths, 4);
    
    job->total_objects = 5000;
    job->migrated_objects = 2500;
    job->state = BUCKETS_MIGRATION_STATE_MIGRATING;
    job->start_time = time(NULL) - 1800;  /* Started 30 minutes ago */
    
    char checkpoint_path[512];
    snprintf(checkpoint_path, sizeof(checkpoint_path), 
             "%s/resume-test.json", g_test_dir);
    
    int ret = buckets_migration_job_save(job, checkpoint_path);
    cr_assert_eq(ret, BUCKETS_OK, "Checkpoint save failed");
    
    /* Cleanup original job */
    buckets_migration_job_cleanup(job);
    
    /* Resume from checkpoint */
    buckets_migration_job_t *resumed = buckets_migration_job_resume_from_checkpoint(
        checkpoint_path, g_old_topology, g_new_topology, g_disk_paths, 4);
    
    cr_assert_not_null(resumed, "Resume from checkpoint failed");
    cr_assert_eq(resumed->total_objects, 5000, "Total objects mismatch");
    cr_assert_eq(resumed->migrated_objects, 2500, "Migrated objects mismatch");
    cr_assert_eq(resumed->state, BUCKETS_MIGRATION_STATE_PAUSED, 
                 "State should be PAUSED after resume");
    cr_assert_not_null(resumed->old_topology, "Old topology should be set");
    cr_assert_not_null(resumed->new_topology, "New topology should be set");
    cr_assert_not_null(resumed->disk_paths, "Disk paths should be set");
    cr_assert_eq(resumed->disk_count, 4, "Disk count mismatch");
    
    buckets_migration_job_cleanup(resumed);
}

/**
 * Test 4: Resume with NULL parameters (error case)
 */
Test(migration_integration, resume_with_null_parameters)
{
    char checkpoint_path[512];
    snprintf(checkpoint_path, sizeof(checkpoint_path), 
             "%s/null-test.json", g_test_dir);
    
    /* NULL checkpoint path */
    buckets_migration_job_t *job1 = buckets_migration_job_resume_from_checkpoint(
        NULL, g_old_topology, g_new_topology, g_disk_paths, 4);
    cr_assert_null(job1, "Should fail with NULL checkpoint path");
    
    /* NULL old topology */
    buckets_migration_job_t *job2 = buckets_migration_job_resume_from_checkpoint(
        checkpoint_path, NULL, g_new_topology, g_disk_paths, 4);
    cr_assert_null(job2, "Should fail with NULL old topology");
    
    /* NULL new topology */
    buckets_migration_job_t *job3 = buckets_migration_job_resume_from_checkpoint(
        checkpoint_path, g_old_topology, NULL, g_disk_paths, 4);
    cr_assert_null(job3, "Should fail with NULL new topology");
    
    /* NULL disk paths */
    buckets_migration_job_t *job4 = buckets_migration_job_resume_from_checkpoint(
        checkpoint_path, g_old_topology, g_new_topology, NULL, 4);
    cr_assert_null(job4, "Should fail with NULL disk paths");
    
    /* Zero disk count */
    buckets_migration_job_t *job5 = buckets_migration_job_resume_from_checkpoint(
        checkpoint_path, g_old_topology, g_new_topology, g_disk_paths, 0);
    cr_assert_null(job5, "Should fail with zero disk count");
}

/**
 * Test 5: Resume from nonexistent checkpoint
 */
Test(migration_integration, resume_from_nonexistent_checkpoint)
{
    buckets_migration_job_t *job = buckets_migration_job_resume_from_checkpoint(
        "/tmp/nonexistent-checkpoint-file.json",
        g_old_topology, g_new_topology, g_disk_paths, 4);
    
    cr_assert_null(job, "Should fail with nonexistent checkpoint file");
}

/**
 * Test 6: Checkpoint path initialization on job start
 */
Test(migration_integration, checkpoint_time_initialized_on_start)
{
    buckets_migration_job_t *job = buckets_migration_job_create(
        1, 2, g_old_topology, g_new_topology, g_disk_paths, 4);
    
    cr_assert_eq(job->last_checkpoint_time, 0, "Checkpoint time should be 0 before start");
    
    /* Start job (will fail scan due to no objects, but that's OK) */
    time_t before_start = time(NULL);
    buckets_migration_job_start(job);
    time_t after_start = time(NULL);
    
    /* Verify checkpoint time was initialized */
    cr_assert_geq(job->last_checkpoint_time, before_start, 
                  "Checkpoint time should be >= start time");
    cr_assert_leq(job->last_checkpoint_time, after_start,
                  "Checkpoint time should be <= current time");
    cr_assert_eq(job->last_checkpoint_objects, 0,
                 "Checkpoint objects should be 0 at start");
    
    buckets_migration_job_cleanup(job);
}

/**
 * Test 7: Multiple checkpoints with different states
 */
Test(migration_integration, checkpoint_multiple_states)
{
    char checkpoint_path[512];
    snprintf(checkpoint_path, sizeof(checkpoint_path), 
             "%s/states-test.json", g_test_dir);
    
    /* Test each state */
    buckets_migration_state_t states[] = {
        BUCKETS_MIGRATION_STATE_IDLE,
        BUCKETS_MIGRATION_STATE_SCANNING,
        BUCKETS_MIGRATION_STATE_MIGRATING,
        BUCKETS_MIGRATION_STATE_PAUSED,
        BUCKETS_MIGRATION_STATE_COMPLETED,
        BUCKETS_MIGRATION_STATE_FAILED
    };
    
    for (int i = 0; i < 6; i++) {
        buckets_migration_job_t *job = buckets_migration_job_create(
            1, 2, g_old_topology, g_new_topology, g_disk_paths, 4);
        
        job->state = states[i];
        job->total_objects = 1000 * (i + 1);
        job->migrated_objects = 100 * i;
        
        /* Save */
        int ret = buckets_migration_job_save(job, checkpoint_path);
        cr_assert_eq(ret, BUCKETS_OK, "Save failed for state %d", states[i]);
        
        /* Load */
        buckets_migration_job_t *loaded = buckets_migration_job_load(checkpoint_path);
        cr_assert_not_null(loaded, "Load failed for state %d", states[i]);
        cr_assert_eq(loaded->state, states[i], "State mismatch for state %d", states[i]);
        cr_assert_eq(loaded->total_objects, 1000 * (i + 1), "Total mismatch for state %d", states[i]);
        cr_assert_eq(loaded->migrated_objects, 100 * i, "Migrated mismatch for state %d", states[i]);
        
        buckets_migration_job_cleanup(job);
        buckets_migration_job_cleanup(loaded);
    }
}

/**
 * Test 8: Checkpoint path format
 */
Test(migration_integration, checkpoint_path_format)
{
    buckets_migration_job_t *job = buckets_migration_job_create(
        42, 43, g_old_topology, g_new_topology, g_disk_paths, 4);
    
    /* Verify job ID format */
    cr_assert_str_eq(job->job_id, "migration-gen-42-to-43", "Job ID format incorrect");
    
    /* Verify checkpoint path contains job ID */
    cr_assert(strstr(job->checkpoint_path, "migration-gen-42-to-43") != NULL,
              "Checkpoint path should contain job ID");
    cr_assert(strstr(job->checkpoint_path, ".checkpoint") != NULL,
              "Checkpoint path should have .checkpoint extension");
    
    buckets_migration_job_cleanup(job);
}

/**
 * Test 9: Job cleanup doesn't crash with checkpoint fields
 */
Test(migration_integration, cleanup_with_checkpoint_fields)
{
    buckets_migration_job_t *job = buckets_migration_job_create(
        1, 2, g_old_topology, g_new_topology, g_disk_paths, 4);
    
    /* Set checkpoint fields */
    job->last_checkpoint_time = time(NULL);
    job->last_checkpoint_objects = 1000;
    
    /* Should cleanup without crashing */
    buckets_migration_job_cleanup(job);
    
    /* Test passes if no crash */
    cr_assert(true, "Cleanup successful");
}

/**
 * Test 10: Large numbers in checkpoint
 */
Test(migration_integration, checkpoint_large_numbers)
{
    buckets_migration_job_t *job = buckets_migration_job_create(
        1, 2, g_old_topology, g_new_topology, g_disk_paths, 4);
    
    /* Set large numbers */
    job->total_objects = 1000000000LL;  /* 1 billion */
    job->migrated_objects = 500000000LL;  /* 500 million */
    job->failed_objects = 12345LL;
    job->bytes_total = 100LL * 1024 * 1024 * 1024;  /* 100 GB */
    job->bytes_migrated = 50LL * 1024 * 1024 * 1024;  /* 50 GB */
    
    char checkpoint_path[512];
    snprintf(checkpoint_path, sizeof(checkpoint_path), 
             "%s/large-numbers.json", g_test_dir);
    
    /* Save */
    int ret = buckets_migration_job_save(job, checkpoint_path);
    cr_assert_eq(ret, BUCKETS_OK, "Save with large numbers failed");
    
    /* Load */
    buckets_migration_job_t *loaded = buckets_migration_job_load(checkpoint_path);
    cr_assert_not_null(loaded, "Load with large numbers failed");
    
    /* Verify */
    cr_assert_eq(loaded->total_objects, 1000000000LL, "Total objects mismatch");
    cr_assert_eq(loaded->migrated_objects, 500000000LL, "Migrated objects mismatch");
    cr_assert_eq(loaded->failed_objects, 12345LL, "Failed objects mismatch");
    cr_assert_eq(loaded->bytes_total, 100LL * 1024 * 1024 * 1024, "Bytes total mismatch");
    cr_assert_eq(loaded->bytes_migrated, 50LL * 1024 * 1024 * 1024, "Bytes migrated mismatch");
    
    buckets_migration_job_cleanup(job);
    buckets_migration_job_cleanup(loaded);
}
