/**
 * Test Async I/O Module
 * 
 * Tests for libuv-based asynchronous disk I/O.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buckets.h"
#include "../src/net/async_io.h"

/* Test directory */
#define TEST_DIR "/tmp/buckets_async_test"

/* Test state */
typedef struct {
    int completed;
    int status;
    void *data;
    size_t size;
    uv_loop_t *loop;
} test_state_t;

/* ===================================================================
 * Test Helpers
 * ===================================================================*/

static void cleanup_test_dir(void)
{
    /* Simple cleanup - remove files we know about */
    unlink(TEST_DIR "/test_write.txt");
    unlink(TEST_DIR "/test_atomic.txt");
    unlink(TEST_DIR "/subdir/nested.txt");
    unlink(TEST_DIR "/chunks/object1/part.1");
    unlink(TEST_DIR "/chunks/object1/part.2");
    rmdir(TEST_DIR "/subdir");
    rmdir(TEST_DIR "/chunks/object1");
    rmdir(TEST_DIR "/chunks");
    rmdir(TEST_DIR);
}

static void setup_test_dir(void)
{
    cleanup_test_dir();
    mkdir(TEST_DIR, 0755);
}

/* ===================================================================
 * Write Tests
 * ===================================================================*/

static void write_complete_cb(async_write_req_t *req, int status)
{
    test_state_t *state = (test_state_t*)req->user_data;
    state->completed = 1;
    state->status = status;
    uv_stop(state->loop);
}

static int test_simple_write(void)
{
    setup_test_dir();
    
    uv_loop_t loop;
    uv_loop_init(&loop);
    
    async_io_ctx_t ctx;
    if (async_io_init(&ctx, &loop) != 0) {
        printf("FAIL: Failed to init async_io\n");
        return 1;
    }
    
    test_state_t state = {0};
    state.loop = &loop;
    
    const char *data = "Hello, async world!";
    char path[256];
    snprintf(path, sizeof(path), "%s/test_write.txt", TEST_DIR);
    
    int ret = async_io_write_file(&ctx, path, data, strlen(data), 
                                   false, false, write_complete_cb, &state);
    if (ret != 0) {
        printf("FAIL: Failed to queue write\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    /* Run event loop until complete */
    uv_run(&loop, UV_RUN_DEFAULT);
    
    if (!state.completed) {
        printf("FAIL: Write did not complete\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    if (state.status != 0) {
        printf("FAIL: Write failed with status %d\n", state.status);
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    /* Verify file exists and content is correct */
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("FAIL: File not created\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    
    if (strcmp(buf, data) != 0) {
        printf("FAIL: Content mismatch: '%s' vs '%s'\n", buf, data);
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    async_io_cleanup(&ctx);
    uv_loop_close(&loop);
    
    printf("PASS: test_simple_write\n");
    return 0;
}

static int test_atomic_write(void)
{
    setup_test_dir();
    
    uv_loop_t loop;
    uv_loop_init(&loop);
    
    async_io_ctx_t ctx;
    async_io_init(&ctx, &loop);
    
    test_state_t state = {0};
    state.loop = &loop;
    
    const char *data = "Atomic write test data";
    char path[256];
    snprintf(path, sizeof(path), "%s/test_atomic.txt", TEST_DIR);
    
    int ret = async_io_write_file(&ctx, path, data, strlen(data),
                                   false, true, write_complete_cb, &state);
    if (ret != 0) {
        printf("FAIL: Failed to queue atomic write\n");
        return 1;
    }
    
    uv_run(&loop, UV_RUN_DEFAULT);
    
    if (state.status != 0) {
        printf("FAIL: Atomic write failed\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    /* Verify no temp file left behind */
    char tmp_path[280];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (access(tmp_path, F_OK) == 0) {
        printf("FAIL: Temp file left behind\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    /* Verify content */
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("FAIL: File not created\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    
    if (strcmp(buf, data) != 0) {
        printf("FAIL: Content mismatch\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    async_io_cleanup(&ctx);
    uv_loop_close(&loop);
    
    printf("PASS: test_atomic_write\n");
    return 0;
}

/* ===================================================================
 * Read Tests
 * ===================================================================*/

static void read_complete_cb(async_read_req_t *req, int status, 
                             void *data, size_t size)
{
    test_state_t *state = (test_state_t*)req->user_data;
    state->completed = 1;
    state->status = status;
    state->data = data;
    state->size = size;
    uv_stop(state->loop);
}

static int test_read_file(void)
{
    setup_test_dir();
    
    /* Create test file */
    const char *expected = "Read test content";
    char path[256];
    snprintf(path, sizeof(path), "%s/test_read.txt", TEST_DIR);
    
    FILE *f = fopen(path, "w");
    fwrite(expected, 1, strlen(expected), f);
    fclose(f);
    
    uv_loop_t loop;
    uv_loop_init(&loop);
    
    async_io_ctx_t ctx;
    async_io_init(&ctx, &loop);
    
    test_state_t state = {0};
    state.loop = &loop;
    
    int ret = async_io_read_file(&ctx, path, 0, read_complete_cb, &state);
    if (ret != 0) {
        printf("FAIL: Failed to queue read\n");
        return 1;
    }
    
    uv_run(&loop, UV_RUN_DEFAULT);
    
    if (state.status != 0) {
        printf("FAIL: Read failed with status %d\n", state.status);
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    if (state.size != strlen(expected)) {
        printf("FAIL: Size mismatch: %zu vs %zu\n", state.size, strlen(expected));
        buckets_free(state.data);
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    if (memcmp(state.data, expected, state.size) != 0) {
        printf("FAIL: Content mismatch\n");
        buckets_free(state.data);
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    buckets_free(state.data);
    async_io_cleanup(&ctx);
    uv_loop_close(&loop);
    
    /* Cleanup */
    unlink(path);
    
    printf("PASS: test_read_file\n");
    return 0;
}

/* ===================================================================
 * Chunk Tests
 * ===================================================================*/

static void chunk_write_cb(void *user_data, int status)
{
    test_state_t *state = (test_state_t*)user_data;
    state->completed = 1;
    state->status = status;
    uv_stop(state->loop);
}

static void chunk_read_cb(void *user_data, int status, void *data, size_t size)
{
    test_state_t *state = (test_state_t*)user_data;
    state->completed = 1;
    state->status = status;
    state->data = data;
    state->size = size;
    uv_stop(state->loop);
}

static int test_chunk_write_read(void)
{
    setup_test_dir();
    
    uv_loop_t loop;
    uv_loop_init(&loop);
    
    async_io_ctx_t ctx;
    async_io_init(&ctx, &loop);
    
    test_state_t state = {0};
    state.loop = &loop;
    
    /* Write chunk */
    const char *chunk_data = "This is chunk data for testing";
    char disk_path[256];
    snprintf(disk_path, sizeof(disk_path), "%s/chunks", TEST_DIR);
    
    int ret = async_io_write_chunk(&ctx, disk_path, "object1", 1,
                                    chunk_data, strlen(chunk_data),
                                    false, chunk_write_cb, &state);
    if (ret != 0) {
        printf("FAIL: Failed to queue chunk write\n");
        return 1;
    }
    
    uv_run(&loop, UV_RUN_DEFAULT);
    
    if (state.status != 0) {
        printf("FAIL: Chunk write failed\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    /* Verify chunk file exists */
    char chunk_path[512];
    snprintf(chunk_path, sizeof(chunk_path), "%s/chunks/object1/part.1", TEST_DIR);
    if (access(chunk_path, F_OK) != 0) {
        printf("FAIL: Chunk file not created at %s\n", chunk_path);
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    /* Read chunk back */
    state.completed = 0;
    state.status = -1;
    state.data = NULL;
    state.size = 0;
    
    ret = async_io_read_chunk(&ctx, disk_path, "object1", 1,
                               chunk_read_cb, &state);
    if (ret != 0) {
        printf("FAIL: Failed to queue chunk read\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    uv_run(&loop, UV_RUN_DEFAULT);
    
    if (state.status != 0) {
        printf("FAIL: Chunk read failed\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    if (state.size != strlen(chunk_data)) {
        printf("FAIL: Chunk size mismatch\n");
        buckets_free(state.data);
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    if (memcmp(state.data, chunk_data, state.size) != 0) {
        printf("FAIL: Chunk content mismatch\n");
        buckets_free(state.data);
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    buckets_free(state.data);
    async_io_cleanup(&ctx);
    uv_loop_close(&loop);
    
    printf("PASS: test_chunk_write_read\n");
    return 0;
}

/* ===================================================================
 * Batch Tests
 * ===================================================================*/

static void batch_complete_cb(void *user_data, int num_success, int num_failed)
{
    test_state_t *state = (test_state_t*)user_data;
    state->completed = 1;
    state->status = (num_failed == 0) ? 0 : -1;
    state->size = (size_t)num_success;  /* Repurpose size for success count */
    uv_stop(state->loop);
}

static int test_batch_write(void)
{
    setup_test_dir();
    
    uv_loop_t loop;
    uv_loop_init(&loop);
    
    async_io_ctx_t ctx;
    async_io_init(&ctx, &loop);
    
    test_state_t state = {0};
    state.loop = &loop;
    
    /* Create batch for 3 writes */
    async_batch_write_t *batch = async_io_batch_write_start(&ctx, 3,
                                                             batch_complete_cb, &state);
    if (!batch) {
        printf("FAIL: Failed to create batch\n");
        return 1;
    }
    
    /* Add writes */
    char path1[256], path2[256], path3[256];
    snprintf(path1, sizeof(path1), "%s/batch1.txt", TEST_DIR);
    snprintf(path2, sizeof(path2), "%s/batch2.txt", TEST_DIR);
    snprintf(path3, sizeof(path3), "%s/batch3.txt", TEST_DIR);
    
    async_io_batch_write_add(batch, path1, "content1", 8, false, true);
    async_io_batch_write_add(batch, path2, "content2", 8, false, true);
    async_io_batch_write_add(batch, path3, "content3", 8, false, true);
    
    uv_run(&loop, UV_RUN_DEFAULT);
    
    if (state.status != 0) {
        printf("FAIL: Batch write failed\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    if (state.size != 3) {
        printf("FAIL: Expected 3 successes, got %zu\n", state.size);
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    /* Verify all files exist */
    if (access(path1, F_OK) != 0 ||
        access(path2, F_OK) != 0 ||
        access(path3, F_OK) != 0) {
        printf("FAIL: Not all batch files created\n");
        async_io_cleanup(&ctx);
        uv_loop_close(&loop);
        return 1;
    }
    
    /* Cleanup */
    unlink(path1);
    unlink(path2);
    unlink(path3);
    
    async_io_cleanup(&ctx);
    uv_loop_close(&loop);
    
    printf("PASS: test_batch_write\n");
    return 0;
}

/* ===================================================================
 * Main
 * ===================================================================*/

int main(void)
{
    printf("=== Async I/O Tests ===\n\n");
    
    int failures = 0;
    
    failures += test_simple_write();
    failures += test_atomic_write();
    failures += test_read_file();
    failures += test_chunk_write_read();
    failures += test_batch_write();
    
    cleanup_test_dir();
    
    printf("\n=== Results: %d failures ===\n", failures);
    
    return failures > 0 ? 1 : 0;
}
