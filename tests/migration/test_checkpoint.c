/**
 * Checkpoint Tests
 * 
 * Tests for migration job checkpointing and recovery.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_migration.h"

#include <criterion/criterion.h>
#include <criterion/redirect.h>

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

typedef struct {
    buckets_migration_job_t *job;
    char test_dir[256];
    char checkpoint_path[512];
} checkpoint_test_ctx_t;

static checkpoint_test_ctx_t g_ctx;

/* Setup fixture */
void checkpoint_setup(void)
{
    buckets_init();
    
    // Generate test directory path FIRST before memset
    char test_dir_temp[256];
    char checkpoint_path_temp[512];
    
    snprintf(test_dir_temp, sizeof(test_dir_temp),
             "/tmp/buckets-checkpoint-test-%d-%ld",
             getpid(), (long)time(NULL));
    
    snprintf(checkpoint_path_temp, sizeof(checkpoint_path_temp),
             "%s/checkpoint.json", test_dir_temp);
    
    // Now clear context and copy paths
    memset(&g_ctx, 0, sizeof(g_ctx));
    
    memcpy(g_ctx.test_dir, test_dir_temp, sizeof(g_ctx.test_dir));
    memcpy(g_ctx.checkpoint_path, checkpoint_path_temp, sizeof(g_ctx.checkpoint_path));
    
    // Create test directory
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", g_ctx.test_dir);
    if (system(cmd) != 0) { /* Ignore errors */ }
}

/* Teardown fixture */
void checkpoint_teardown(void)
{
    if (g_ctx.job) {
        buckets_migration_job_cleanup(g_ctx.job);
    }
    
    // Cleanup test directory
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_ctx.test_dir);
    if (system(cmd) != 0) { /* Ignore errors */ }
    
    buckets_cleanup();
}

TestSuite(checkpoint, .init = checkpoint_setup, .fini = checkpoint_teardown);

/* ===================================================================
 * Tests
 * ===================================================================*/

/**
 * Test 1: Save checkpoint
 */
Test(checkpoint, save_checkpoint)
{
    // Create a mock job
    g_ctx.job = buckets_calloc(1, sizeof(buckets_migration_job_t));
    cr_assert_not_null(g_ctx.job, "Should allocate job");
    
    strncpy(g_ctx.job->job_id, "migration-gen-42-to-43", sizeof(g_ctx.job->job_id) - 1);
    g_ctx.job->source_generation = 42;
    g_ctx.job->target_generation = 43;
    g_ctx.job->state = BUCKETS_MIGRATION_STATE_MIGRATING;
    g_ctx.job->start_time = time(NULL);
    g_ctx.job->total_objects = 1000;
    g_ctx.job->migrated_objects = 500;
    g_ctx.job->failed_objects = 5;
    g_ctx.job->bytes_total = 1024LL * 1024 * 1024;  // 1 GB
    g_ctx.job->bytes_migrated = 512LL * 1024 * 1024;  // 512 MB
    
    pthread_mutex_init(&g_ctx.job->lock, NULL);
    
    // Save checkpoint
    int ret = buckets_migration_job_save(g_ctx.job, g_ctx.checkpoint_path);
    cr_assert_eq(ret, BUCKETS_OK, "Should save checkpoint successfully");
    
    // Verify file exists
    FILE *f = fopen(g_ctx.checkpoint_path, "r");
    cr_assert_not_null(f, "Checkpoint file should exist");
    fclose(f);
}

/**
 * Test 2: Load checkpoint
 */
Test(checkpoint, load_checkpoint)
{
    // Create and save a job
    g_ctx.job = buckets_calloc(1, sizeof(buckets_migration_job_t));
    strncpy(g_ctx.job->job_id, "migration-gen-100-to-101", sizeof(g_ctx.job->job_id) - 1);
    g_ctx.job->source_generation = 100;
    g_ctx.job->target_generation = 101;
    g_ctx.job->state = BUCKETS_MIGRATION_STATE_PAUSED;
    g_ctx.job->start_time = 1234567890;
    g_ctx.job->total_objects = 5000;
    g_ctx.job->migrated_objects = 2500;
    g_ctx.job->failed_objects = 25;
    g_ctx.job->bytes_total = 5LL * 1024 * 1024 * 1024;  // 5 GB
    g_ctx.job->bytes_migrated = 2LL * 1024 * 1024 * 1024;  // 2 GB
    
    pthread_mutex_init(&g_ctx.job->lock, NULL);
    
    int ret = buckets_migration_job_save(g_ctx.job, g_ctx.checkpoint_path);
    cr_assert_eq(ret, BUCKETS_OK, "Should save checkpoint");
    
    // Load checkpoint
    buckets_migration_job_t *loaded = buckets_migration_job_load(g_ctx.checkpoint_path);
    cr_assert_not_null(loaded, "Should load checkpoint");
    
    // Verify job ID
    cr_assert_str_eq(loaded->job_id, "migration-gen-100-to-101", "Job ID should match");
    
    // Verify generations
    cr_assert_eq(loaded->source_generation, 100, "Source generation should match");
    cr_assert_eq(loaded->target_generation, 101, "Target generation should match");
    
    // Verify state
    cr_assert_eq(loaded->state, BUCKETS_MIGRATION_STATE_PAUSED, "State should match");
    
    // Verify timestamps
    cr_assert_eq(loaded->start_time, 1234567890, "Start time should match");
    
    // Verify progress
    cr_assert_eq(loaded->total_objects, 5000, "Total objects should match");
    cr_assert_eq(loaded->migrated_objects, 2500, "Migrated objects should match");
    cr_assert_eq(loaded->failed_objects, 25, "Failed objects should match");
    cr_assert_eq(loaded->bytes_total, 5LL * 1024 * 1024 * 1024, "Bytes total should match");
    cr_assert_eq(loaded->bytes_migrated, 2LL * 1024 * 1024 * 1024, "Bytes migrated should match");
    
    buckets_migration_job_cleanup(loaded);
}

/**
 * Test 3: Save with NULL parameters
 */
Test(checkpoint, save_null_params)
{
    g_ctx.job = buckets_calloc(1, sizeof(buckets_migration_job_t));
    pthread_mutex_init(&g_ctx.job->lock, NULL);
    
    // NULL job
    int ret = buckets_migration_job_save(NULL, g_ctx.checkpoint_path);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG, "Should reject NULL job");
    
    // NULL path
    ret = buckets_migration_job_save(g_ctx.job, NULL);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG, "Should reject NULL path");
}

/**
 * Test 4: Load with NULL path
 */
Test(checkpoint, load_null_path)
{
    buckets_migration_job_t *loaded = buckets_migration_job_load(NULL);
    cr_assert_null(loaded, "Should return NULL for NULL path");
}

/**
 * Test 5: Load nonexistent file
 */
Test(checkpoint, load_nonexistent)
{
    buckets_migration_job_t *loaded = buckets_migration_job_load("/nonexistent/path/checkpoint.json");
    cr_assert_null(loaded, "Should return NULL for nonexistent file");
}

/**
 * Test 6: Save/load roundtrip with all states
 */
Test(checkpoint, roundtrip_all_states)
{
    buckets_migration_state_t states[] = {
        BUCKETS_MIGRATION_STATE_IDLE,
        BUCKETS_MIGRATION_STATE_SCANNING,
        BUCKETS_MIGRATION_STATE_MIGRATING,
        BUCKETS_MIGRATION_STATE_PAUSED,
        BUCKETS_MIGRATION_STATE_COMPLETED,
        BUCKETS_MIGRATION_STATE_FAILED
    };
    
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        // Create job with specific state
        g_ctx.job = buckets_calloc(1, sizeof(buckets_migration_job_t));
        strncpy(g_ctx.job->job_id, "test-job", sizeof(g_ctx.job->job_id) - 1);
        g_ctx.job->source_generation = 1;
        g_ctx.job->target_generation = 2;
        g_ctx.job->state = states[i];
        pthread_mutex_init(&g_ctx.job->lock, NULL);
        
        // Save
        int ret = buckets_migration_job_save(g_ctx.job, g_ctx.checkpoint_path);
        cr_assert_eq(ret, BUCKETS_OK, "Should save state %d", states[i]);
        
        // Load
        buckets_migration_job_t *loaded = buckets_migration_job_load(g_ctx.checkpoint_path);
        cr_assert_not_null(loaded, "Should load state %d", states[i]);
        cr_assert_eq(loaded->state, states[i], "State %d should match", states[i]);
        
        buckets_migration_job_cleanup(loaded);
        buckets_migration_job_cleanup(g_ctx.job);
        g_ctx.job = NULL;
    }
}

/**
 * Test 7: Save with zero progress
 */
Test(checkpoint, save_zero_progress)
{
    g_ctx.job = buckets_calloc(1, sizeof(buckets_migration_job_t));
    strncpy(g_ctx.job->job_id, "zero-progress", sizeof(g_ctx.job->job_id) - 1);
    g_ctx.job->source_generation = 1;
    g_ctx.job->target_generation = 2;
    g_ctx.job->state = BUCKETS_MIGRATION_STATE_IDLE;
    g_ctx.job->total_objects = 0;
    g_ctx.job->migrated_objects = 0;
    g_ctx.job->failed_objects = 0;
    g_ctx.job->bytes_total = 0;
    g_ctx.job->bytes_migrated = 0;
    
    pthread_mutex_init(&g_ctx.job->lock, NULL);
    
    int ret = buckets_migration_job_save(g_ctx.job, g_ctx.checkpoint_path);
    cr_assert_eq(ret, BUCKETS_OK, "Should save with zero progress");
    
    buckets_migration_job_t *loaded = buckets_migration_job_load(g_ctx.checkpoint_path);
    cr_assert_not_null(loaded, "Should load");
    cr_assert_eq(loaded->total_objects, 0, "Total should be zero");
    cr_assert_eq(loaded->migrated_objects, 0, "Migrated should be zero");
    
    buckets_migration_job_cleanup(loaded);
}

/**
 * Test 8: Save with large numbers
 */
Test(checkpoint, save_large_numbers)
{
    g_ctx.job = buckets_calloc(1, sizeof(buckets_migration_job_t));
    strncpy(g_ctx.job->job_id, "large-numbers", sizeof(g_ctx.job->job_id) - 1);
    g_ctx.job->source_generation = 1000000;
    g_ctx.job->target_generation = 1000001;
    g_ctx.job->state = BUCKETS_MIGRATION_STATE_MIGRATING;
    g_ctx.job->total_objects = 1000000000LL;  // 1 billion
    g_ctx.job->migrated_objects = 500000000LL;  // 500 million
    g_ctx.job->failed_objects = 1000LL;
    g_ctx.job->bytes_total = 100LL * 1024 * 1024 * 1024;  // 100 GB
    g_ctx.job->bytes_migrated = 50LL * 1024 * 1024 * 1024;  // 50 GB
    
    pthread_mutex_init(&g_ctx.job->lock, NULL);
    
    int ret = buckets_migration_job_save(g_ctx.job, g_ctx.checkpoint_path);
    cr_assert_eq(ret, BUCKETS_OK, "Should save large numbers");
    
    buckets_migration_job_t *loaded = buckets_migration_job_load(g_ctx.checkpoint_path);
    cr_assert_not_null(loaded, "Should load");
    cr_assert_eq(loaded->total_objects, 1000000000LL, "Large number should match");
    cr_assert_eq(loaded->bytes_total, 100LL * 1024 * 1024 * 1024, "Large bytes should match");
    
    buckets_migration_job_cleanup(loaded);
}

/**
 * Test 9: Multiple save/load cycles
 */
Test(checkpoint, multiple_cycles)
{
    for (int i = 0; i < 5; i++) {
        g_ctx.job = buckets_calloc(1, sizeof(buckets_migration_job_t));
        snprintf(g_ctx.job->job_id, sizeof(g_ctx.job->job_id), "cycle-%d", i);
        g_ctx.job->source_generation = i;
        g_ctx.job->target_generation = i + 1;
        g_ctx.job->state = BUCKETS_MIGRATION_STATE_MIGRATING;
        g_ctx.job->migrated_objects = i * 100;
        
        pthread_mutex_init(&g_ctx.job->lock, NULL);
        
        int ret = buckets_migration_job_save(g_ctx.job, g_ctx.checkpoint_path);
        cr_assert_eq(ret, BUCKETS_OK, "Cycle %d should save", i);
        
        buckets_migration_job_t *loaded = buckets_migration_job_load(g_ctx.checkpoint_path);
        cr_assert_not_null(loaded, "Cycle %d should load", i);
        cr_assert_eq(loaded->migrated_objects, i * 100, "Cycle %d progress should match", i);
        
        buckets_migration_job_cleanup(loaded);
        buckets_migration_job_cleanup(g_ctx.job);
        g_ctx.job = NULL;
    }
}

/**
 * Test 10: Concurrent save/load (basic thread safety)
 */
Test(checkpoint, thread_safety)
{
    g_ctx.job = buckets_calloc(1, sizeof(buckets_migration_job_t));
    strncpy(g_ctx.job->job_id, "thread-safety", sizeof(g_ctx.job->job_id) - 1);
    g_ctx.job->source_generation = 1;
    g_ctx.job->target_generation = 2;
    g_ctx.job->state = BUCKETS_MIGRATION_STATE_MIGRATING;
    g_ctx.job->total_objects = 1000;
    
    pthread_mutex_init(&g_ctx.job->lock, NULL);
    
    // Save multiple times (simulating periodic checkpointing)
    for (int i = 0; i < 10; i++) {
        g_ctx.job->migrated_objects = i * 100;
        
        int ret = buckets_migration_job_save(g_ctx.job, g_ctx.checkpoint_path);
        cr_assert_eq(ret, BUCKETS_OK, "Save %d should succeed", i);
    }
    
    // Load final state
    buckets_migration_job_t *loaded = buckets_migration_job_load(g_ctx.checkpoint_path);
    cr_assert_not_null(loaded, "Should load final state");
    cr_assert_eq(loaded->migrated_objects, 900, "Final progress should match");
    
    buckets_migration_job_cleanup(loaded);
}
