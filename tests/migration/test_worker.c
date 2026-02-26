/**
 * Migration Worker Pool Tests
 * 
 * Tests for parallel object migration with thread pool.
 */

#include <criterion/criterion.h>
#include <criterion/redirect.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>  /* For getpid */

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
    buckets_worker_pool_t *pool;
    char test_dir[256];
} worker_test_ctx_t;

static worker_test_ctx_t g_ctx;

/* Setup fixture */
void worker_setup(void)
{
    buckets_init();
    
    /* Generate unique test directory */
    snprintf(g_ctx.test_dir, sizeof(g_ctx.test_dir),
             "/tmp/buckets-worker-test-%d-%ld",
             getpid(), (long)time(NULL));
    
    memset(&g_ctx, 0, sizeof(g_ctx));
}

/* Teardown fixture */
void worker_teardown(void)
{
    if (g_ctx.pool) {
        buckets_worker_pool_free(g_ctx.pool);
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

TestSuite(worker_pool, .init = worker_setup, .fini = worker_teardown);

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

/* ===================================================================
 * Tests
 * ===================================================================*/

/**
 * Test 1: Worker pool creation
 */
Test(worker_pool, create_pool)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.pool = buckets_worker_pool_create(4, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    
    cr_assert_not_null(g_ctx.pool, "Worker pool should be created");
}

/**
 * Test 2: Worker pool creation with NULL arguments
 */
Test(worker_pool, create_null_args)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    /* NULL old_topology */
    buckets_worker_pool_t *pool1 = buckets_worker_pool_create(4, NULL, g_ctx.new_topo,
                                                                g_ctx.disk_paths, g_ctx.disk_count);
    cr_assert_null(pool1, "Should reject NULL old_topology");
    
    /* NULL new_topology */
    buckets_worker_pool_t *pool2 = buckets_worker_pool_create(4, g_ctx.old_topo, NULL,
                                                                g_ctx.disk_paths, g_ctx.disk_count);
    cr_assert_null(pool2, "Should reject NULL new_topology");
    
    /* NULL disk_paths */
    buckets_worker_pool_t *pool3 = buckets_worker_pool_create(4, g_ctx.old_topo, g_ctx.new_topo,
                                                                NULL, g_ctx.disk_count);
    cr_assert_null(pool3, "Should reject NULL disk_paths");
}

/**
 * Test 3: Start worker threads
 */
Test(worker_pool, start_workers)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.pool = buckets_worker_pool_create(4, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    cr_assert_not_null(g_ctx.pool);
    
    int ret = buckets_worker_pool_start(g_ctx.pool);
    cr_assert_eq(ret, BUCKETS_OK, "Should start workers successfully");
}

/**
 * Test 4: Submit empty task queue
 */
Test(worker_pool, submit_empty)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.pool = buckets_worker_pool_create(4, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    buckets_worker_pool_start(g_ctx.pool);
    
    /* Submit 0 tasks */
    buckets_migration_task_t tasks[1];
    int ret = buckets_worker_pool_submit(g_ctx.pool, tasks, 0);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG, "Should reject empty task array");
}

/**
 * Test 5: Submit single task
 */
Test(worker_pool, submit_single_task)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.pool = buckets_worker_pool_create(4, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    buckets_worker_pool_start(g_ctx.pool);
    
    /* Create task */
    buckets_migration_task_t task = {
        .old_pool_idx = 0,
        .old_set_idx = 0,
        .new_pool_idx = 1,
        .new_set_idx = 0,
        .size = 1024,
        .retry_count = 0
    };
    snprintf(task.bucket, sizeof(task.bucket), "test-bucket");
    snprintf(task.object, sizeof(task.object), "test-object");
    
    int ret = buckets_worker_pool_submit(g_ctx.pool, &task, 1);
    cr_assert_eq(ret, BUCKETS_OK, "Should submit task successfully");
    
    /* Wait for completion */
    buckets_worker_pool_wait(g_ctx.pool);
}

/**
 * Test 6: Submit multiple tasks
 */
Test(worker_pool, submit_multiple_tasks)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.pool = buckets_worker_pool_create(8, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    buckets_worker_pool_start(g_ctx.pool);
    
    /* Create 10 tasks */
    buckets_migration_task_t tasks[10];
    for (int i = 0; i < 10; i++) {
        tasks[i].old_pool_idx = 0;
        tasks[i].old_set_idx = 0;
        tasks[i].new_pool_idx = 1;
        tasks[i].new_set_idx = 0;
        tasks[i].size = 1024 * (i + 1);
        tasks[i].retry_count = 0;
        
        snprintf(tasks[i].bucket, sizeof(tasks[i].bucket), "test-bucket");
        snprintf(tasks[i].object, sizeof(tasks[i].object), "test-object-%d", i);
    }
    
    int ret = buckets_worker_pool_submit(g_ctx.pool, tasks, 10);
    cr_assert_eq(ret, BUCKETS_OK, "Should submit 10 tasks");
    
    /* Wait for completion */
    buckets_worker_pool_wait(g_ctx.pool);
}

/**
 * Test 7: Get worker statistics
 */
Test(worker_pool, get_stats)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.pool = buckets_worker_pool_create(4, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    buckets_worker_pool_start(g_ctx.pool);
    
    /* Create 5 tasks */
    buckets_migration_task_t tasks[5];
    for (int i = 0; i < 5; i++) {
        tasks[i].old_pool_idx = 0;
        tasks[i].old_set_idx = 0;
        tasks[i].new_pool_idx = 1;
        tasks[i].new_set_idx = 0;
        tasks[i].size = 1024;
        tasks[i].retry_count = 0;
        
        snprintf(tasks[i].bucket, sizeof(tasks[i].bucket), "test-bucket");
        snprintf(tasks[i].object, sizeof(tasks[i].object), "test-object-%d", i);
    }
    
    buckets_worker_pool_submit(g_ctx.pool, tasks, 5);
    buckets_worker_pool_wait(g_ctx.pool);
    
    /* Get stats */
    buckets_worker_stats_t stats;
    int ret = buckets_worker_pool_get_stats(g_ctx.pool, &stats);
    cr_assert_eq(ret, BUCKETS_OK, "Should get stats");
    cr_assert_eq(stats.tasks_completed, 5, "Should have completed 5 tasks");
    cr_assert_eq(stats.bytes_migrated, 5120, "Should have migrated 5120 bytes");
}

/**
 * Test 8: Stop worker pool
 */
Test(worker_pool, stop_pool)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.pool = buckets_worker_pool_create(4, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    buckets_worker_pool_start(g_ctx.pool);
    
    int ret = buckets_worker_pool_stop(g_ctx.pool);
    cr_assert_eq(ret, BUCKETS_OK, "Should stop workers");
}

/**
 * Test 9: Large task batch
 */
Test(worker_pool, large_batch)
{
    g_ctx.disk_paths = create_test_disks(8);
    g_ctx.disk_count = 8;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 8, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 8, 2, 2);
    
    g_ctx.pool = buckets_worker_pool_create(16, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    buckets_worker_pool_start(g_ctx.pool);
    
    /* Create 100 tasks */
    buckets_migration_task_t *tasks = buckets_calloc(100, sizeof(buckets_migration_task_t));
    for (int i = 0; i < 100; i++) {
        tasks[i].old_pool_idx = 0;
        tasks[i].old_set_idx = i % 2;
        tasks[i].new_pool_idx = 1;
        tasks[i].new_set_idx = i % 2;
        tasks[i].size = 1024;
        tasks[i].retry_count = 0;
        
        snprintf(tasks[i].bucket, sizeof(tasks[i].bucket), "bucket-%d", i / 10);
        snprintf(tasks[i].object, sizeof(tasks[i].object), "object-%d", i);
    }
    
    int ret = buckets_worker_pool_submit(g_ctx.pool, tasks, 100);
    cr_assert_eq(ret, BUCKETS_OK, "Should submit 100 tasks");
    
    buckets_worker_pool_wait(g_ctx.pool);
    
    /* Verify stats */
    buckets_worker_stats_t stats;
    buckets_worker_pool_get_stats(g_ctx.pool, &stats);
    cr_assert_eq(stats.tasks_completed, 100, "Should complete 100 tasks");
    
    buckets_free(tasks);
}

/**
 * Test 10: Default worker count
 */
Test(worker_pool, default_worker_count)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    /* Create with 0 or negative workers - should use default (16) */
    g_ctx.pool = buckets_worker_pool_create(0, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    cr_assert_not_null(g_ctx.pool, "Should create with default worker count");
    
    buckets_worker_pool_start(g_ctx.pool);
    buckets_worker_pool_stop(g_ctx.pool);
}

/**
 * Test 11: Worker pool cleanup
 */
Test(worker_pool, cleanup)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.pool = buckets_worker_pool_create(4, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    buckets_worker_pool_start(g_ctx.pool);
    
    /* Free should stop and cleanup */
    buckets_worker_pool_free(g_ctx.pool);
    g_ctx.pool = NULL;  /* Avoid double-free in teardown */
}

/**
 * Test 12: Submit before start (should fail)
 */
Test(worker_pool, submit_before_start)
{
    g_ctx.disk_paths = create_test_disks(4);
    g_ctx.disk_count = 4;
    
    g_ctx.old_topo = create_test_topology(g_ctx.disk_paths, 4, 1, 2);
    g_ctx.new_topo = create_test_topology(g_ctx.disk_paths, 4, 2, 2);
    
    g_ctx.pool = buckets_worker_pool_create(4, g_ctx.old_topo, g_ctx.new_topo,
                                             g_ctx.disk_paths, g_ctx.disk_count);
    
    /* Try to submit before starting */
    buckets_migration_task_t task = {
        .old_pool_idx = 0,
        .old_set_idx = 0,
        .new_pool_idx = 1,
        .new_set_idx = 0,
        .size = 1024
    };
    
    int ret = buckets_worker_pool_submit(g_ctx.pool, &task, 1);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG, "Should reject submit before start");
}
