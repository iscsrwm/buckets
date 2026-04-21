/**
 * Group Commit Tests
 * 
 * Unit tests for batched fsync functionality.
 */

#include <criterion/criterion.h>
#include <criterion/parameterized.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>

#include "buckets.h"
#include "buckets_group_commit.h"

/* Test fixture */
static buckets_group_commit_context_t *ctx = NULL;
static const char *test_file = "/tmp/test_group_commit.dat";

void setup(void) {
    buckets_init();
    unlink(test_file);  /* Clean up from previous tests */
}

void teardown(void) {
    if (ctx) {
        buckets_group_commit_cleanup(ctx);
        ctx = NULL;
    }
    unlink(test_file);
    buckets_cleanup();
}

TestSuite(group_commit, .init = setup, .fini = teardown);

/* ===================================================================
 * Initialization Tests
 * ===================================================================*/

Test(group_commit, init_with_defaults) {
    ctx = buckets_group_commit_init(NULL);
    cr_assert_not_null(ctx, "Context should be initialized");
    
    const buckets_group_commit_config_t *config = buckets_group_commit_get_config(ctx);
    cr_assert_not_null(config);
    cr_assert_eq(config->batch_size, BUCKETS_GC_DEFAULT_BATCH_SIZE);
    cr_assert_eq(config->batch_time_ms, BUCKETS_GC_DEFAULT_BATCH_TIME_MS);
    cr_assert(config->use_fdatasync);
    cr_assert_eq(config->durability, BUCKETS_DURABILITY_BATCHED);
}

Test(group_commit, init_with_custom_config) {
    buckets_group_commit_config_t config = {
        .batch_size = 32,
        .batch_time_ms = 5,
        .use_fdatasync = false,
        .durability = BUCKETS_DURABILITY_IMMEDIATE
    };
    
    ctx = buckets_group_commit_init(&config);
    cr_assert_not_null(ctx);
    
    const buckets_group_commit_config_t *retrieved = buckets_group_commit_get_config(ctx);
    cr_assert_eq(retrieved->batch_size, 32);
    cr_assert_eq(retrieved->batch_time_ms, 5);
    cr_assert_not(retrieved->use_fdatasync);
    cr_assert_eq(retrieved->durability, BUCKETS_DURABILITY_IMMEDIATE);
}

Test(group_commit, default_config) {
    buckets_group_commit_config_t config = buckets_group_commit_default_config();
    cr_assert_eq(config.batch_size, BUCKETS_GC_DEFAULT_BATCH_SIZE);
    cr_assert_eq(config.batch_time_ms, BUCKETS_GC_DEFAULT_BATCH_TIME_MS);
    cr_assert(config.use_fdatasync);
    cr_assert_eq(config.durability, BUCKETS_DURABILITY_BATCHED);
}

Test(group_commit, config_for_durability_none) {
    buckets_group_commit_config_t config = 
        buckets_group_commit_config_for_durability(BUCKETS_DURABILITY_NONE);
    cr_assert_eq(config.durability, BUCKETS_DURABILITY_NONE);
    cr_assert_eq(config.batch_size, 0);
}

Test(group_commit, config_for_durability_immediate) {
    buckets_group_commit_config_t config = 
        buckets_group_commit_config_for_durability(BUCKETS_DURABILITY_IMMEDIATE);
    cr_assert_eq(config.durability, BUCKETS_DURABILITY_IMMEDIATE);
    cr_assert_eq(config.batch_size, 1);
}

/* ===================================================================
 * Write Tests
 * ===================================================================*/

Test(group_commit, single_write) {
    ctx = buckets_group_commit_init(NULL);
    
    int fd = open(test_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    cr_assert_geq(fd, 0);
    
    const char *data = "Hello, World!";
    ssize_t written = buckets_group_commit_write(ctx, fd, data, strlen(data));
    cr_assert_eq(written, strlen(data));
    
    close(fd);
    
    /* Verify data was written */
    fd = open(test_file, O_RDONLY);
    char buf[64] = {0};
    ssize_t r = read(fd, buf, sizeof(buf));
    (void)r;  /* Suppress unused result warning */
    close(fd);
    
    cr_assert_str_eq(buf, data);
}

Test(group_commit, pwrite_with_offset) {
    ctx = buckets_group_commit_init(NULL);
    
    int fd = open(test_file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    cr_assert_geq(fd, 0);
    
    /* Write at offset 0 */
    const char *data1 = "AAAA";
    ssize_t written = buckets_group_commit_pwrite(ctx, fd, data1, 4, 0);
    cr_assert_eq(written, 4);
    
    /* Write at offset 10 */
    const char *data2 = "BBBB";
    written = buckets_group_commit_pwrite(ctx, fd, data2, 4, 10);
    cr_assert_eq(written, 4);
    
    buckets_group_commit_flush_fd(ctx, fd);
    close(fd);
    
    /* Verify */
    fd = open(test_file, O_RDONLY);
    char buf[20] = {0};
    ssize_t r = read(fd, buf, sizeof(buf));
    (void)r;  /* Suppress unused result warning */
    close(fd);
    
    cr_assert_eq(memcmp(buf, "AAAA", 4), 0);
    cr_assert_eq(memcmp(buf + 10, "BBBB", 4), 0);
}

Test(group_commit, multiple_writes_batched) {
    buckets_group_commit_config_t config = buckets_group_commit_default_config();
    config.batch_size = 10;  /* Batch every 10 writes */
    ctx = buckets_group_commit_init(&config);
    
    int fd = open(test_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    cr_assert_geq(fd, 0);
    
    /* Write 5 times (should not trigger batch) */
    for (int i = 0; i < 5; i++) {
        char data[16];
        snprintf(data, sizeof(data), "Line %d\n", i);
        ssize_t written = buckets_group_commit_write(ctx, fd, data, strlen(data));
        cr_assert_gt(written, 0);
    }
    
    /* Get stats - should have 5 writes, no syncs yet */
    buckets_group_commit_stats_t stats;
    buckets_group_commit_get_stats(ctx, &stats);
    cr_assert_eq(stats.total_writes, 5);
    
    /* Write 5 more (should trigger batch at 10) */
    for (int i = 5; i < 10; i++) {
        char data[16];
        snprintf(data, sizeof(data), "Line %d\n", i);
        ssize_t written = buckets_group_commit_write(ctx, fd, data, strlen(data));
        cr_assert_gt(written, 0);
    }
    
    /* Get stats - should have 10 writes, 1 sync */
    buckets_group_commit_get_stats(ctx, &stats);
    cr_assert_eq(stats.total_writes, 10);
    cr_assert_geq(stats.total_syncs, 1);
    
    close(fd);
}

/* ===================================================================
 * Flush Tests
 * ===================================================================*/

Test(group_commit, explicit_flush_fd) {
    ctx = buckets_group_commit_init(NULL);
    
    int fd = open(test_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    cr_assert_geq(fd, 0);
    
    /* Write some data */
    const char *data = "Test data";
    buckets_group_commit_write(ctx, fd, data, strlen(data));
    
    /* Explicit flush */
    int ret = buckets_group_commit_flush_fd(ctx, fd);
    cr_assert_eq(ret, 0);
    
    /* Check stats */
    buckets_group_commit_stats_t stats;
    buckets_group_commit_get_stats(ctx, &stats);
    cr_assert_geq(stats.explicit_flushes, 1);
    
    close(fd);
}

Test(group_commit, flush_all) {
    ctx = buckets_group_commit_init(NULL);
    
    /* Open multiple files */
    int fd1 = open("/tmp/test_gc_1.dat", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int fd2 = open("/tmp/test_gc_2.dat", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    cr_assert_geq(fd1, 0);
    cr_assert_geq(fd2, 0);
    
    /* Write to both */
    buckets_group_commit_write(ctx, fd1, "Data 1", 6);
    buckets_group_commit_write(ctx, fd2, "Data 2", 6);
    
    /* Flush all */
    int ret = buckets_group_commit_flush_all(ctx);
    cr_assert_eq(ret, 0);
    
    close(fd1);
    close(fd2);
    unlink("/tmp/test_gc_1.dat");
    unlink("/tmp/test_gc_2.dat");
}

/* ===================================================================
 * Durability Level Tests
 * ===================================================================*/

Test(group_commit, durability_none_no_sync) {
    buckets_group_commit_config_t config = 
        buckets_group_commit_config_for_durability(BUCKETS_DURABILITY_NONE);
    ctx = buckets_group_commit_init(&config);
    
    int fd = open(test_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    cr_assert_geq(fd, 0);
    
    /* Write many times */
    for (int i = 0; i < 100; i++) {
        char data[16];
        snprintf(data, sizeof(data), "Data %d\n", i);
        buckets_group_commit_write(ctx, fd, data, strlen(data));
    }
    
    /* Check stats - should have no syncs */
    buckets_group_commit_stats_t stats;
    buckets_group_commit_get_stats(ctx, &stats);
    cr_assert_eq(stats.total_writes, 100);
    cr_assert_eq(stats.total_syncs, 0);
    
    close(fd);
}

Test(group_commit, durability_immediate_always_sync) {
    buckets_group_commit_config_t config = 
        buckets_group_commit_config_for_durability(BUCKETS_DURABILITY_IMMEDIATE);
    ctx = buckets_group_commit_init(&config);
    
    int fd = open(test_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    cr_assert_geq(fd, 0);
    
    /* Write 10 times */
    for (int i = 0; i < 10; i++) {
        char data[16];
        snprintf(data, sizeof(data), "Data %d\n", i);
        buckets_group_commit_write(ctx, fd, data, strlen(data));
    }
    
    /* Check stats - should have 10 syncs (one per write) */
    buckets_group_commit_stats_t stats;
    buckets_group_commit_get_stats(ctx, &stats);
    cr_assert_eq(stats.total_writes, 10);
    cr_assert_geq(stats.total_syncs, 10);
    
    close(fd);
}

/* ===================================================================
 * Statistics Tests
 * ===================================================================*/

Test(group_commit, stats_tracking) {
    ctx = buckets_group_commit_init(NULL);
    
    int fd = open(test_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    cr_assert_geq(fd, 0);
    
    /* Write and track */
    for (int i = 0; i < 5; i++) {
        buckets_group_commit_write(ctx, fd, "test", 4);
    }
    
    buckets_group_commit_stats_t stats;
    buckets_group_commit_get_stats(ctx, &stats);
    
    cr_assert_eq(stats.total_writes, 5);
    cr_assert_eq(stats.bytes_written, 20);
    
    close(fd);
}

Test(group_commit, stats_reset) {
    ctx = buckets_group_commit_init(NULL);
    
    int fd = open(test_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    cr_assert_geq(fd, 0);
    
    /* Write some data */
    buckets_group_commit_write(ctx, fd, "test", 4);
    
    /* Reset stats */
    buckets_group_commit_reset_stats(ctx);
    
    /* Check stats are zero */
    buckets_group_commit_stats_t stats;
    buckets_group_commit_get_stats(ctx, &stats);
    cr_assert_eq(stats.total_writes, 0);
    cr_assert_eq(stats.total_syncs, 0);
    cr_assert_eq(stats.bytes_written, 0);
    
    close(fd);
}

/* ===================================================================
 * Error Handling Tests
 * ===================================================================*/

Test(group_commit, null_context) {
    ssize_t ret = buckets_group_commit_write(NULL, 1, "data", 4);
    cr_assert_eq(ret, -1);
    
    ret = buckets_group_commit_pwrite(NULL, 1, "data", 4, 0);
    cr_assert_eq(ret, -1);
}

Test(group_commit, invalid_fd) {
    ctx = buckets_group_commit_init(NULL);
    
    ssize_t ret = buckets_group_commit_write(ctx, -1, "data", 4);
    cr_assert_eq(ret, -1);
}
