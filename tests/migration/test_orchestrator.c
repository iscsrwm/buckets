/**
 * Migration Orchestrator Tests
 * 
 * Tests for job management and state machine.
 */

#include <criterion/criterion.h>
#include <criterion/redirect.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_migration.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

typedef struct {
    char **disk_paths;
    int disk_count;
    buckets_cluster_topology_t *old_topo;
    buckets_cluster_topology_t *new_topo;
    buckets_migration_job_t *job;
    char test_dir[256];
    int callback_count;
} orchestrator_test_ctx_t;

static orchestrator_test_ctx_t g_ctx;

/* Setup fixture */
void orchestrator_setup(void)
{
    buckets_init();
    
    /* Generate unique test directory */
    snprintf(g_ctx.test_dir, sizeof(g_ctx.test_dir),
             "/tmp/buckets-orchestrator-test-%d-%ld",
             getpid(), (long)time(NULL));
    
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.callback_count = 0;
}

/* Teardown fixture */
void orchestrator_teardown(void)
{
    if (g_ctx.job) {
        buckets_migration_job_cleanup(g_ctx.job);
    }
    if (g_ctx.old_topo) {
        buckets_topology_free(g_ctx.old_topo);
    }
    if (g_ctx.new_topo) {
        buckets_topology_free(g_ctx.new_topo);
    }
    if (g_ctx.disk_paths) {
        for (int i = 0; i < g_ctx.disk_count; i++) {
            buckets_free(g_ctx.disk_paths[i]);
        }
        buckets_free(g_ctx.disk_paths);
    }
    
    /* Cleanup test directory */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_ctx.test_dir);
    if (system(cmd) != 0) { /* Ignore errors */ }
    
    buckets_cleanup();
}

TestSuite(orchestrator, .init = orchestrator_setup, .fini = orchestrator_teardown);

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/**
 * Create test disk paths
 */
static char** create_test_disks(int count)
{
    char **disk_paths = buckets_calloc(count, sizeof(char*));
    for (int i = 0; i < count; i++) {
        disk_paths[i] = buckets_format("/tmp/buckets-test/disk%d", i);
        
        /* Create directory */
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "mkdir -p %s", disk_paths[i]);
        if (system(cmd) != 0) { /* Ignore errors */ }
    }
    return disk_paths;
}

/**
 * Create simple test topology
 */
static buckets_cluster_topology_t* create_test_topology(char **disk_paths, 
                                                         int disk_count,
                                                         int pool_count, 
                                                         int sets_per_pool)
{
    buckets_cluster_topology_t *topology = buckets_topology_new();
    topology->version = 1;
    
    int disk_idx = 0;
    for (int p = 0; p < pool_count; p++) {
        buckets_topology_add_pool(topology);
        for (int s = 0; s < sets_per_pool; s++) {
            int disks_per_set = disk_count / (pool_count * sets_per_pool);
            buckets_disk_info_t *disks = buckets_calloc(disks_per_set, sizeof(buckets_disk_info_t));
            
            for (int d = 0; d < disks_per_set; d++) {
                snprintf(disks[d].endpoint, sizeof(disks[d].endpoint), 
                         "http://localhost:9000%s", disk_paths[disk_idx % disk_count]);
                snprintf(disks[d].uuid, sizeof(disks[d].uuid), 
                         "disk-%d-%d-%d", p, s, d);
                disks[d].capacity = 1024ULL * 1024 * 1024 * 1024;  /* 1TB */
                disk_idx++;
            }
            
            buckets_topology_add_set(topology, p, disks, disks_per_set);
            buckets_free(disks);
        }
    }
    
    return topology;
}

/**
 * Event callback for testing
 */
static void test_event_callback(buckets_migration_job_t *job,
                                 const char *event_type,
                                 void *user_data)
{
    (void)job;
    (void)event_type;
    
    orchestrator_test_ctx_t *ctx = (orchestrator_test_ctx_t*)user_data;
    ctx->callback_count++;
}

/* ===================================================================
 * Tests
 * ===================================================================*/

/**
 * Test 1: Job creation
 */
Test(orchestrator, create_job)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    cr_assert_not_null(g_ctx.job, "Job should be created");
    cr_assert_eq(g_ctx.job->source_generation, 42, "Source generation should be 42");
    cr_assert_eq(g_ctx.job->target_generation, 43, "Target generation should be 43");
    cr_assert_eq(g_ctx.job->state, BUCKETS_MIGRATION_STATE_IDLE, "Initial state should be IDLE");
}

/**
 * Test 2: Job creation with NULL arguments
 */
Test(orchestrator, create_job_null_args)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    /* NULL old_topology */
    buckets_migration_job_t *job1 = buckets_migration_job_create(42, 43, NULL, g_ctx.new_topo,
                                                                   g_ctx.disk_paths, g_ctx.disk_count);
    cr_assert_null(job1, "Should reject NULL old_topology");
    
    /* NULL new_topology */
    buckets_migration_job_t *job2 = buckets_migration_job_create(42, 43, g_ctx.old_topo, NULL,
                                                                   g_ctx.disk_paths, g_ctx.disk_count);
    cr_assert_null(job2, "Should reject NULL new_topology");
    
    /* NULL disk_paths */
    buckets_migration_job_t *job3 = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                                                   NULL, g_ctx.disk_count);
    cr_assert_null(job3, "Should reject NULL disk_paths");
}

/**
 * Test 3: Get job state
 */
Test(orchestrator, get_state)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    buckets_migration_state_t state = buckets_migration_job_get_state(g_ctx.job);
    cr_assert_eq(state, BUCKETS_MIGRATION_STATE_IDLE, "Initial state should be IDLE");
}

/**
 * Test 4: Start job (empty migration - no objects)
 */
Test(orchestrator, start_job_empty)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    int ret = buckets_migration_job_start(g_ctx.job);
    cr_assert_eq(ret, BUCKETS_OK, "Start should succeed");
    
    /* Should transition IDLE -> SCANNING -> MIGRATING -> COMPLETED */
    /* Since no objects, should complete immediately */
    buckets_migration_state_t state = buckets_migration_job_get_state(g_ctx.job);
    cr_assert_eq(state, BUCKETS_MIGRATION_STATE_COMPLETED, 
                 "Should be COMPLETED (no objects to migrate)");
}

/**
 * Test 5: Get progress
 */
Test(orchestrator, get_progress)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    i64 total, completed, failed;
    double percent;
    i64 eta;
    
    int ret = buckets_migration_job_get_progress(g_ctx.job, &total, &completed, &failed, 
                                                   &percent, &eta);
    cr_assert_eq(ret, BUCKETS_OK, "Get progress should succeed");
    cr_assert_eq(total, 0, "Total should be 0 before start");
    cr_assert_eq(completed, 0, "Completed should be 0 before start");
    cr_assert_eq(failed, 0, "Failed should be 0 before start");
    cr_assert_float_eq(percent, 0.0, 0.01, "Percent should be 0");
}

/**
 * Test 6: Set event callback
 */
Test(orchestrator, set_callback)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    int ret = buckets_migration_job_set_callback(g_ctx.job, test_event_callback, &g_ctx);
    cr_assert_eq(ret, BUCKETS_OK, "Set callback should succeed");
    
    /* Start job to trigger state change callback */
    g_ctx.callback_count = 0;
    buckets_migration_job_start(g_ctx.job);
    
    /* Should have fired callback at least once (state change) */
    cr_assert_geq(g_ctx.callback_count, 1, "Callback should be fired");
}

/**
 * Test 7: Stop job
 */
Test(orchestrator, stop_job)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    buckets_migration_job_start(g_ctx.job);
    
    int ret = buckets_migration_job_stop(g_ctx.job);
    cr_assert_eq(ret, BUCKETS_OK, "Stop should succeed");
    
    buckets_migration_state_t state = buckets_migration_job_get_state(g_ctx.job);
    /* Job may complete before stop is called, so accept terminal states */
    cr_assert(state == BUCKETS_MIGRATION_STATE_FAILED || 
              state == BUCKETS_MIGRATION_STATE_COMPLETED,
              "State should be terminal (FAILED or COMPLETED) after stop");
}

/**
 * Test 8: Wait for completion
 */
Test(orchestrator, wait_completion)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    buckets_migration_job_start(g_ctx.job);
    
    int ret = buckets_migration_job_wait(g_ctx.job);
    cr_assert_eq(ret, BUCKETS_OK, "Wait should succeed");
    
    /* Should be in terminal state */
    buckets_migration_state_t state = buckets_migration_job_get_state(g_ctx.job);
    cr_assert(state == BUCKETS_MIGRATION_STATE_COMPLETED || 
              state == BUCKETS_MIGRATION_STATE_FAILED,
              "Should be in terminal state");
}

/**
 * Test 9: Job cleanup
 */
Test(orchestrator, cleanup_job)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    /* Cleanup should work */
    buckets_migration_job_cleanup(g_ctx.job);
    g_ctx.job = NULL;  /* Avoid double-free in teardown */
}

/**
 * Test 10: Invalid state transitions
 */
Test(orchestrator, invalid_transitions)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    /* Try to pause from IDLE (invalid) */
    int ret = buckets_migration_job_pause(g_ctx.job);
    cr_assert_neq(ret, BUCKETS_OK, "Pause from IDLE should fail");
    
    /* Try to resume from IDLE (invalid) */
    ret = buckets_migration_job_resume(g_ctx.job);
    cr_assert_neq(ret, BUCKETS_OK, "Resume from IDLE should fail");
}

/**
 * Test 11: Job ID format
 */
Test(orchestrator, job_id_format)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    /* Check job ID format */
    cr_assert_str_eq(g_ctx.job->job_id, "migration-gen-42-to-43", 
                     "Job ID should have correct format");
}

/**
 * Test 12: Progress percentage calculation
 */
Test(orchestrator, progress_percentage)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    /* Manually set progress for testing */
    g_ctx.job->total_objects = 100;
    g_ctx.job->migrated_objects = 25;
    
    double percent;
    buckets_migration_job_get_progress(g_ctx.job, NULL, NULL, NULL, &percent, NULL);
    
    cr_assert_float_eq(percent, 25.0, 0.01, "Percentage should be 25%");
}

/**
 * Test 13: Multiple topology generations
 */
Test(orchestrator, multiple_generations)
{
    g_ctx.disk_paths = create_test_disks(8);
    g_ctx.disk_count = 8;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 8, 2, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 8, 3, 2);
    
    g_ctx.job = buckets_migration_job_create(100, 101, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    cr_assert_not_null(g_ctx.job, "Job should be created");
    cr_assert_eq(g_ctx.job->source_generation, 100, "Source generation should be 100");
    cr_assert_eq(g_ctx.job->target_generation, 101, "Target generation should be 101");
}

/**
 * Test 14: Job save/load (placeholders)
 */
Test(orchestrator, job_persistence)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.job = buckets_migration_job_create(42, 43, g_ctx.old_topo, g_ctx.new_topo,
                                              g_ctx.disk_paths, g_ctx.disk_count);
    
    /* Save should work (placeholder) */
    int ret = buckets_migration_job_save(g_ctx.job, "/tmp/test-job.json");
    cr_assert_eq(ret, BUCKETS_OK, "Save should succeed (placeholder)");
    
    /* Load should work (placeholder) */
    buckets_migration_job_t *loaded = buckets_migration_job_load("/tmp/test-job.json");
    /* Currently returns NULL (not implemented) */
    cr_assert_null(loaded, "Load returns NULL (not implemented yet)");
}
