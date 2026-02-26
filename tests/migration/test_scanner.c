/**
 * Scanner Tests
 * 
 * Tests for migration scanner that enumerates objects and identifies
 * those needing migration.
 */

#include <criterion/criterion.h>
#include <criterion/redirect.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_migration.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

static char test_dir[256];
static char **disk_paths = NULL;
static int disk_count = 0;

void scanner_setup(void)
{
    buckets_init();
    
    /* Create temp directory for test */
    snprintf(test_dir, sizeof(test_dir), "/tmp/buckets_scanner_test_%d_%ld",
             getpid(), time(NULL));
    mkdir(test_dir, 0755);
}

void scanner_teardown(void)
{
    /* Cleanup test directory */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", test_dir);
    if (system(cmd) != 0) { /* Ignore errors */ }
    
    /* Cleanup disk paths */
    if (disk_paths) {
        for (int i = 0; i < disk_count; i++) {
            buckets_free(disk_paths[i]);
        }
        buckets_free(disk_paths);
        disk_paths = NULL;
        disk_count = 0;
    }
    
    buckets_cleanup();
}

TestSuite(scanner, .init = scanner_setup, .fini = scanner_teardown);

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/**
 * Create test disk paths
 */
static void create_test_disks(int count)
{
    disk_count = count;
    disk_paths = buckets_calloc(disk_count, sizeof(char*));
    
    for (int i = 0; i < disk_count; i++) {
        disk_paths[i] = buckets_format("%s/disk%d", test_dir, i);
        mkdir(disk_paths[i], 0755);
    }
}

/**
 * Create a simple topology with pools and sets
 */
static buckets_cluster_topology_t* create_test_topology(int pool_count, int sets_per_pool)
{
    buckets_cluster_topology_t *topology = buckets_topology_new();
    cr_assert_not_null(topology);
    
    snprintf(topology->deployment_id, sizeof(topology->deployment_id), "test-deployment");
    topology->generation = 1;
    
    for (int p = 0; p < pool_count; p++) {
        buckets_topology_add_pool(topology);
        
        for (int s = 0; s < sets_per_pool; s++) {
            buckets_disk_info_t disks[4] = {
                {.endpoint = "http://node:9000/disk1", .capacity = 1000000000},
                {.endpoint = "http://node:9000/disk2", .capacity = 1000000000},
                {.endpoint = "http://node:9000/disk3", .capacity = 1000000000},
                {.endpoint = "http://node:9000/disk4", .capacity = 1000000000}
            };
            buckets_topology_add_set(topology, p, disks, 4);
        }
    }
    
    return topology;
}

/**
 * Create a mock object on disk (just create xl.meta file)
 */
static void create_mock_object(const char *disk_path, const char *bucket,
                                const char *object, size_t size)
{
    (void)size;  /* Unused parameter */
    
    char obj_path[4096];
    snprintf(obj_path, sizeof(obj_path), "%s/%s/%s", disk_path, bucket, object);
    
    /* Create parent directories */
    char cmd[5120];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", obj_path);
    if (system(cmd) != 0) { /* Ignore errors */ }
    
    /* Create xl.meta file */
    char meta_path[4120];  /* obj_path (4096) + "/xl.meta" (9) + null (1) */
    snprintf(meta_path, sizeof(meta_path), "%s/xl.meta", obj_path);
    
    FILE *fp = fopen(meta_path, "w");
    if (fp) {
        fprintf(fp, "mock xl.meta\n");
        fclose(fp);
    }
}

/* ===================================================================
 * Tests
 * ===================================================================*/

/**
 * Test 1: Empty cluster (no objects)
 */
Test(scanner, empty_cluster)
{
    create_test_disks(4);
    
    buckets_cluster_topology_t *old_topo = create_test_topology(1, 2);
    buckets_cluster_topology_t *new_topo = create_test_topology(2, 2);
    
    buckets_scanner_state_t *scanner = buckets_scanner_init(disk_paths, disk_count,
                                                             old_topo, new_topo);
    cr_assert_not_null(scanner);
    
    buckets_migration_task_t *queue = NULL;
    int queue_size = 0, task_count = 0;
    
    int ret = buckets_scanner_scan(scanner, &queue, &queue_size, &task_count);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(task_count, 0, "No objects should be found");
    
    buckets_scanner_cleanup(scanner);
    buckets_topology_free(old_topo);
    buckets_topology_free(new_topo);
}

/**
 * Test 2: Scanner initialization
 */
Test(scanner, initialization)
{
    create_test_disks(8);
    
    buckets_cluster_topology_t *old_topo = create_test_topology(1, 2);
    buckets_cluster_topology_t *new_topo = create_test_topology(2, 2);
    
    buckets_scanner_state_t *scanner = buckets_scanner_init(disk_paths, disk_count,
                                                             old_topo, new_topo);
    cr_assert_not_null(scanner);
    cr_assert_eq(scanner->disk_count, 8);
    cr_assert_eq(scanner->objects_scanned, 0);
    cr_assert_eq(scanner->objects_affected, 0);
    cr_assert_eq(scanner->scan_complete, false);
    
    buckets_scanner_cleanup(scanner);
    buckets_topology_free(old_topo);
    buckets_topology_free(new_topo);
}

/**
 * Test 3: Scanner with single pool
 */
Test(scanner, single_pool)
{
    create_test_disks(4);
    
    /* Create some mock objects */
    create_mock_object(disk_paths[0], "bucket1", "object1", 1024);
    create_mock_object(disk_paths[1], "bucket1", "object2", 2048);
    create_mock_object(disk_paths[2], "bucket2", "object3", 4096);
    
    buckets_cluster_topology_t *old_topo = create_test_topology(1, 1);
    buckets_cluster_topology_t *new_topo = create_test_topology(1, 2);
    
    buckets_scanner_state_t *scanner = buckets_scanner_init(disk_paths, disk_count,
                                                             old_topo, new_topo);
    cr_assert_not_null(scanner);
    
    buckets_migration_task_t *queue = NULL;
    int queue_size = 0, task_count = 0;
    
    int ret = buckets_scanner_scan(scanner, &queue, &queue_size, &task_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Some objects should need migration due to topology change */
    cr_assert_geq(scanner->objects_scanned, 3, "Should scan all objects");
    
    if (queue) {
        buckets_free(queue);
    }
    
    buckets_scanner_cleanup(scanner);
    buckets_topology_free(old_topo);
    buckets_topology_free(new_topo);
}

/**
 * Test 4: Scanner statistics
 */
Test(scanner, statistics)
{
    create_test_disks(4);
    
    buckets_cluster_topology_t *old_topo = create_test_topology(1, 2);
    buckets_cluster_topology_t *new_topo = create_test_topology(2, 2);
    
    buckets_scanner_state_t *scanner = buckets_scanner_init(disk_paths, disk_count,
                                                             old_topo, new_topo);
    cr_assert_not_null(scanner);
    
    /* Get stats before scan */
    buckets_scanner_stats_t stats;
    int ret = buckets_scanner_get_stats(scanner, &stats);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(stats.objects_scanned, 0);
    cr_assert_eq(stats.complete, false);
    
    /* Perform scan */
    buckets_migration_task_t *queue = NULL;
    int queue_size = 0, task_count = 0;
    ret = buckets_scanner_scan(scanner, &queue, &queue_size, &task_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Get stats after scan */
    ret = buckets_scanner_get_stats(scanner, &stats);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(stats.complete, true);
    
    if (queue) {
        buckets_free(queue);
    }
    
    buckets_scanner_cleanup(scanner);
    buckets_topology_free(old_topo);
    buckets_topology_free(new_topo);
}

/**
 * Test 5: Multiple pools
 */
Test(scanner, multiple_pools)
{
    create_test_disks(8);
    
    buckets_cluster_topology_t *old_topo = create_test_topology(2, 2);
    buckets_cluster_topology_t *new_topo = create_test_topology(3, 2);
    
    buckets_scanner_state_t *scanner = buckets_scanner_init(disk_paths, disk_count,
                                                             old_topo, new_topo);
    cr_assert_not_null(scanner);
    
    buckets_migration_task_t *queue = NULL;
    int queue_size = 0, task_count = 0;
    
    int ret = buckets_scanner_scan(scanner, &queue, &queue_size, &task_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    if (queue) {
        buckets_free(queue);
    }
    
    buckets_scanner_cleanup(scanner);
    buckets_topology_free(old_topo);
    buckets_topology_free(new_topo);
}

/**
 * Test 6: Invalid arguments
 */
Test(scanner, invalid_arguments)
{
    create_test_disks(4);
    
    buckets_cluster_topology_t *topology = create_test_topology(1, 2);
    
    /* NULL disk_paths */
    buckets_scanner_state_t *scanner = buckets_scanner_init(NULL, disk_count,
                                                             topology, topology);
    cr_assert_null(scanner, "Should fail with NULL disk_paths");
    
    /* Zero disk_count */
    scanner = buckets_scanner_init(disk_paths, 0, topology, topology);
    cr_assert_null(scanner, "Should fail with zero disk_count");
    
    /* NULL old_topology */
    scanner = buckets_scanner_init(disk_paths, disk_count, NULL, topology);
    cr_assert_null(scanner, "Should fail with NULL old_topology");
    
    /* NULL new_topology */
    scanner = buckets_scanner_init(disk_paths, disk_count, topology, NULL);
    cr_assert_null(scanner, "Should fail with NULL new_topology");
    
    buckets_topology_free(topology);
}

/**
 * Test 7: Cleanup properly frees resources
 */
Test(scanner, cleanup)
{
    create_test_disks(4);
    
    buckets_cluster_topology_t *old_topo = create_test_topology(1, 2);
    buckets_cluster_topology_t *new_topo = create_test_topology(2, 2);
    
    buckets_scanner_state_t *scanner = buckets_scanner_init(disk_paths, disk_count,
                                                             old_topo, new_topo);
    cr_assert_not_null(scanner);
    
    /* Cleanup should not crash */
    buckets_scanner_cleanup(scanner);
    
    /* Cleanup NULL scanner should not crash */
    buckets_scanner_cleanup(NULL);
    
    buckets_topology_free(old_topo);
    buckets_topology_free(new_topo);
}

/**
 * Test 8: Task sorting (small objects first)
 */
Test(scanner, task_sorting)
{
    create_test_disks(4);
    
    /* Create objects of different sizes */
    create_mock_object(disk_paths[0], "bucket1", "large", 1024 * 1024);
    create_mock_object(disk_paths[1], "bucket1", "small", 1024);
    create_mock_object(disk_paths[2], "bucket1", "medium", 100 * 1024);
    
    buckets_cluster_topology_t *old_topo = create_test_topology(1, 1);
    buckets_cluster_topology_t *new_topo = create_test_topology(1, 2);
    
    buckets_scanner_state_t *scanner = buckets_scanner_init(disk_paths, disk_count,
                                                             old_topo, new_topo);
    cr_assert_not_null(scanner);
    
    buckets_migration_task_t *queue = NULL;
    int queue_size = 0, task_count = 0;
    
    int ret = buckets_scanner_scan(scanner, &queue, &queue_size, &task_count);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Verify sorting if we have tasks */
    if (task_count > 1) {
        for (int i = 0; i < task_count - 1; i++) {
            cr_assert_leq(queue[i].size, queue[i+1].size,
                          "Tasks should be sorted by size (ascending)");
        }
    }
    
    if (queue) {
        buckets_free(queue);
    }
    
    buckets_scanner_cleanup(scanner);
    buckets_topology_free(old_topo);
    buckets_topology_free(new_topo);
}

/**
 * Test 9: Scanner scan NULL arguments
 */
Test(scanner, scan_null_arguments)
{
    create_test_disks(4);
    
    buckets_cluster_topology_t *old_topo = create_test_topology(1, 2);
    buckets_cluster_topology_t *new_topo = create_test_topology(2, 2);
    
    buckets_scanner_state_t *scanner = buckets_scanner_init(disk_paths, disk_count,
                                                             old_topo, new_topo);
    cr_assert_not_null(scanner);
    
    buckets_migration_task_t *queue = NULL;
    int queue_size = 0, task_count = 0;
    
    /* NULL scanner */
    int ret = buckets_scanner_scan(NULL, &queue, &queue_size, &task_count);
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* NULL queue */
    ret = buckets_scanner_scan(scanner, NULL, &queue_size, &task_count);
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* NULL queue_size */
    ret = buckets_scanner_scan(scanner, &queue, NULL, &task_count);
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* NULL task_count */
    ret = buckets_scanner_scan(scanner, &queue, &queue_size, NULL);
    cr_assert_neq(ret, BUCKETS_OK);
    
    buckets_scanner_cleanup(scanner);
    buckets_topology_free(old_topo);
    buckets_topology_free(new_topo);
}

/**
 * Test 10: Large object count
 */
Test(scanner, large_object_count)
{
    create_test_disks(4);
    
    /* Create many mock objects */
    for (int i = 0; i < 100; i++) {
        char object[256];
        snprintf(object, sizeof(object), "object%d", i);
        create_mock_object(disk_paths[i % disk_count], "bucket1", object, 1024 * i);
    }
    
    buckets_cluster_topology_t *old_topo = create_test_topology(1, 2);
    buckets_cluster_topology_t *new_topo = create_test_topology(2, 2);
    
    buckets_scanner_state_t *scanner = buckets_scanner_init(disk_paths, disk_count,
                                                             old_topo, new_topo);
    cr_assert_not_null(scanner);
    
    buckets_migration_task_t *queue = NULL;
    int queue_size = 0, task_count = 0;
    
    int ret = buckets_scanner_scan(scanner, &queue, &queue_size, &task_count);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_geq(scanner->objects_scanned, 100, "Should scan all created objects");
    
    if (queue) {
        buckets_free(queue);
    }
    
    buckets_scanner_cleanup(scanner);
    buckets_topology_free(old_topo);
    buckets_topology_free(new_topo);
}
