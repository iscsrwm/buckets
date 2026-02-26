/**
 * Broadcast Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <criterion/criterion.h>

#include "buckets.h"
#include "buckets_net.h"
#include "cJSON.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

static buckets_conn_pool_t *pool = NULL;
static buckets_rpc_context_t *ctx = NULL;
static buckets_peer_grid_t *grid = NULL;

void setup_broadcast(void)
{
    buckets_init();
    pool = buckets_conn_pool_create(10);
    ctx = buckets_rpc_context_create(pool);
    grid = buckets_peer_grid_create();
}

void teardown_broadcast(void)
{
    if (grid) {
        buckets_peer_grid_free(grid);
    }
    if (ctx) {
        buckets_rpc_context_free(ctx);
    }
    if (pool) {
        buckets_conn_pool_free(pool);
    }
    buckets_cleanup();
}

TestSuite(broadcast, .init = setup_broadcast, .fini = teardown_broadcast);

/* ===================================================================
 * Broadcast Tests
 * ===================================================================*/

Test(broadcast, empty_grid)
{
    /* Broadcast to empty grid */
    buckets_broadcast_result_t *result = NULL;
    int ret = buckets_rpc_broadcast(ctx, grid, "test.method", NULL, &result, 5000);
    
    cr_assert_eq(ret, BUCKETS_OK, "Broadcast should succeed with empty grid");
    cr_assert_not_null(result, "Result should not be NULL");
    cr_assert_eq(result->total, 0, "Total should be 0");
    cr_assert_eq(result->success, 0, "Success count should be 0");
    cr_assert_eq(result->failed, 0, "Failed count should be 0");
    
    /* Cleanup */
    buckets_broadcast_result_free(result);
}

Test(broadcast, null_params)
{
    /* Add peers */
    buckets_peer_grid_add_peer(grid, "http://127.0.0.1:19100");
    
    /* Broadcast with NULL params */
    buckets_broadcast_result_t *result = NULL;
    int ret = buckets_rpc_broadcast(ctx, grid, "test.method", NULL, &result, 5000);
    
    /* Note: This will fail because peers aren't actually running,
     * but we're testing that NULL params is handled correctly */
    cr_assert_eq(ret, BUCKETS_OK, "Broadcast should succeed");
    cr_assert_not_null(result, "Result should not be NULL");
    
    /* Cleanup */
    buckets_broadcast_result_free(result);
}

Test(broadcast, invalid_args)
{
    buckets_broadcast_result_t *result = NULL;
    
    /* NULL context */
    int ret = buckets_rpc_broadcast(NULL, grid, "test.method", NULL, &result, 5000);
    cr_assert_neq(ret, BUCKETS_OK, "Broadcast should fail with NULL context");
    
    /* NULL grid */
    ret = buckets_rpc_broadcast(ctx, NULL, "test.method", NULL, &result, 5000);
    cr_assert_neq(ret, BUCKETS_OK, "Broadcast should fail with NULL grid");
    
    /* NULL method */
    ret = buckets_rpc_broadcast(ctx, grid, NULL, NULL, &result, 5000);
    cr_assert_neq(ret, BUCKETS_OK, "Broadcast should fail with NULL method");
    
    /* NULL result pointer */
    ret = buckets_rpc_broadcast(ctx, grid, "test.method", NULL, NULL, 5000);
    cr_assert_neq(ret, BUCKETS_OK, "Broadcast should fail with NULL result pointer");
}

Test(broadcast, result_free_null)
{
    /* Should not crash */
    buckets_broadcast_result_free(NULL);
}

Test(broadcast, with_params)
{
    /* Add peers */
    buckets_peer_grid_add_peer(grid, "http://127.0.0.1:19101");
    buckets_peer_grid_add_peer(grid, "http://127.0.0.1:19102");
    
    /* Create params */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "key", "value");
    cJSON_AddNumberToObject(params, "count", 42);
    
    /* Broadcast with params */
    buckets_broadcast_result_t *result = NULL;
    int ret = buckets_rpc_broadcast(ctx, grid, "test.method", params, &result, 5000);
    
    cr_assert_eq(ret, BUCKETS_OK, "Broadcast should succeed");
    cr_assert_not_null(result, "Result should not be NULL");
    cr_assert_eq(result->total, 2, "Total should be 2");
    
    /* Note: Both will fail since peers aren't running, but we're testing
     * that the broadcast logic works correctly with params */
    
    /* Cleanup */
    cJSON_Delete(params);
    buckets_broadcast_result_free(result);
}

Test(broadcast, default_timeout)
{
    /* Add peer */
    buckets_peer_grid_add_peer(grid, "http://127.0.0.1:19103");
    
    /* Broadcast with 0 timeout (should use default 5000ms) */
    buckets_broadcast_result_t *result = NULL;
    int ret = buckets_rpc_broadcast(ctx, grid, "test.method", NULL, &result, 0);
    
    cr_assert_eq(ret, BUCKETS_OK, "Broadcast should succeed with default timeout");
    cr_assert_not_null(result, "Result should not be NULL");
    
    /* Cleanup */
    buckets_broadcast_result_free(result);
}
