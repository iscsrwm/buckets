/**
 * Throttle Tests
 * 
 * Tests for bandwidth throttling and rate limiting.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include "buckets.h"
#include "buckets_migration.h"

#include <criterion/criterion.h>
#include <criterion/redirect.h>

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

typedef struct {
    buckets_throttle_t *throttle;
} throttle_test_ctx_t;

static throttle_test_ctx_t g_ctx;

/* Setup fixture */
void throttle_setup(void)
{
    buckets_init();
    memset(&g_ctx, 0, sizeof(g_ctx));
}

/* Teardown fixture */
void throttle_teardown(void)
{
    if (g_ctx.throttle) {
        buckets_throttle_free(g_ctx.throttle);
        g_ctx.throttle = NULL;
    }
    
    buckets_cleanup();
}

TestSuite(throttle, .init = throttle_setup, .fini = throttle_teardown);

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/**
 * Get current time in microseconds
 */
static i64 get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (i64)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/* ===================================================================
 * Tests
 * ===================================================================*/

/**
 * Test 1: Create throttle with default settings
 */
Test(throttle, create_default)
{
    g_ctx.throttle = buckets_throttle_create_default();
    
    cr_assert_not_null(g_ctx.throttle, "Should create throttle");
    cr_assert(buckets_throttle_is_enabled(g_ctx.throttle), "Should be enabled");
    
    i64 rate = buckets_throttle_get_rate(g_ctx.throttle);
    cr_assert_eq(rate, 100LL * 1024 * 1024, "Should have 100 MB/s default rate");
}

/**
 * Test 2: Create throttle with custom settings
 */
Test(throttle, create_custom)
{
    // 50 MB/s rate, 5 MB burst
    g_ctx.throttle = buckets_throttle_create(50, 5);
    
    cr_assert_not_null(g_ctx.throttle, "Should create throttle");
    
    i64 rate = buckets_throttle_get_rate(g_ctx.throttle);
    cr_assert_eq(rate, 50LL * 1024 * 1024, "Should have 50 MB/s rate");
}

/**
 * Test 3: Create throttle with zero rate (unlimited)
 */
Test(throttle, create_unlimited)
{
    // 0 MB/s = unlimited
    g_ctx.throttle = buckets_throttle_create(0, 0);
    
    cr_assert_not_null(g_ctx.throttle, "Should create throttle");
    cr_assert(!buckets_throttle_is_enabled(g_ctx.throttle), 
              "Should be disabled with zero rate");
}

/**
 * Test 4: Enable/disable throttling
 */
Test(throttle, enable_disable)
{
    g_ctx.throttle = buckets_throttle_create(100, 10);
    
    cr_assert(buckets_throttle_is_enabled(g_ctx.throttle), "Should start enabled");
    
    // Disable
    int ret = buckets_throttle_set_enabled(g_ctx.throttle, false);
    cr_assert_eq(ret, BUCKETS_OK, "Should disable successfully");
    cr_assert(!buckets_throttle_is_enabled(g_ctx.throttle), "Should be disabled");
    
    // Re-enable
    ret = buckets_throttle_set_enabled(g_ctx.throttle, true);
    cr_assert_eq(ret, BUCKETS_OK, "Should enable successfully");
    cr_assert(buckets_throttle_is_enabled(g_ctx.throttle), "Should be enabled");
}

/**
 * Test 5: Set throttle rate dynamically
 */
Test(throttle, set_rate)
{
    g_ctx.throttle = buckets_throttle_create(100, 10);
    
    i64 rate = buckets_throttle_get_rate(g_ctx.throttle);
    cr_assert_eq(rate, 100LL * 1024 * 1024, "Should have initial rate");
    
    // Change to 50 MB/s
    int ret = buckets_throttle_set_rate(g_ctx.throttle, 50LL * 1024 * 1024);
    cr_assert_eq(ret, BUCKETS_OK, "Should set rate successfully");
    
    rate = buckets_throttle_get_rate(g_ctx.throttle);
    cr_assert_eq(rate, 50LL * 1024 * 1024, "Should have new rate");
}

/**
 * Test 6: Throttle wait with small bytes (immediate)
 */
Test(throttle, wait_small)
{
    // 100 MB/s rate, 10 MB burst
    g_ctx.throttle = buckets_throttle_create(100, 10);
    
    i64 start_us = get_time_us();
    
    // Request 1 KB (should be immediate from burst)
    int ret = buckets_throttle_wait(g_ctx.throttle, 1024);
    
    i64 elapsed_us = get_time_us() - start_us;
    
    cr_assert_eq(ret, BUCKETS_OK, "Should succeed");
    cr_assert_lt(elapsed_us, 10000, "Should be fast (<10ms)");
}

/**
 * Test 7: Throttle wait with disabled throttling
 */
Test(throttle, wait_disabled)
{
    g_ctx.throttle = buckets_throttle_create(100, 10);
    
    // Disable throttling
    buckets_throttle_set_enabled(g_ctx.throttle, false);
    
    i64 start_us = get_time_us();
    
    // Request 100 MB (should be immediate when disabled)
    int ret = buckets_throttle_wait(g_ctx.throttle, 100LL * 1024 * 1024);
    
    i64 elapsed_us = get_time_us() - start_us;
    
    cr_assert_eq(ret, BUCKETS_OK, "Should succeed");
    cr_assert_lt(elapsed_us, 10000, "Should be fast (<10ms) when disabled");
}

/**
 * Test 8: Throttle wait with rate limiting (small delay)
 */
Test(throttle, wait_rate_limited)
{
    // 10 MB/s rate, 1 MB burst
    g_ctx.throttle = buckets_throttle_create(10, 1);
    
    // Consume initial burst
    buckets_throttle_wait(g_ctx.throttle, 1024 * 1024);
    
    i64 start_us = get_time_us();
    
    // Request 1 MB (should take ~100ms at 10 MB/s)
    int ret = buckets_throttle_wait(g_ctx.throttle, 1024 * 1024);
    
    i64 elapsed_us = get_time_us() - start_us;
    
    cr_assert_eq(ret, BUCKETS_OK, "Should succeed");
    
    // Should take at least 90ms (allowing for timing variance)
    cr_assert_geq(elapsed_us, 90000, "Should be throttled (>=90ms)");
    
    // Should not take more than 200ms (reasonable upper bound)
    cr_assert_leq(elapsed_us, 200000, "Should not be too slow (<=200ms)");
}

/**
 * Test 9: Get throttle statistics
 */
Test(throttle, get_stats)
{
    g_ctx.throttle = buckets_throttle_create(100, 10);
    
    i64 tokens, rate;
    bool enabled;
    
    int ret = buckets_throttle_get_stats(g_ctx.throttle, &tokens, &rate, &enabled);
    
    cr_assert_eq(ret, BUCKETS_OK, "Should get stats successfully");
    cr_assert_eq(rate, 100LL * 1024 * 1024, "Should report correct rate");
    cr_assert(enabled, "Should be enabled");
    cr_assert_eq(tokens, 10LL * 1024 * 1024, "Should start with full burst");
}

/**
 * Test 10: Token refill over time
 */
Test(throttle, token_refill)
{
    // 10 MB/s rate, 10 MB burst
    g_ctx.throttle = buckets_throttle_create(10, 10);
    
    // Consume all tokens
    buckets_throttle_wait(g_ctx.throttle, 10LL * 1024 * 1024);
    
    i64 tokens_before;
    buckets_throttle_get_stats(g_ctx.throttle, &tokens_before, NULL, NULL);
    cr_assert_leq(tokens_before, 100000, "Should have very few tokens after consuming burst");
    
    // Sleep 100ms (should refill ~1 MB at 10 MB/s)
    struct timespec ts = {0, 100000000L};  // 100ms
    nanosleep(&ts, NULL);
    
    i64 tokens_after;
    buckets_throttle_get_stats(g_ctx.throttle, &tokens_after, NULL, NULL);
    
    // Should have refilled approximately 1 MB (1024*1024 bytes)
    // Allow ±20% variance for timing
    cr_assert_geq(tokens_after, 800000, "Should have refilled some tokens");
    cr_assert_leq(tokens_after, 1300000, "Should not have too many tokens");
}

/**
 * Test 11: Multiple sequential throttle waits
 */
Test(throttle, multiple_waits)
{
    // 20 MB/s rate, 2 MB burst
    g_ctx.throttle = buckets_throttle_create(20, 2);
    
    i64 start_us = get_time_us();
    
    // Request 1 MB three times (3 MB total)
    // First request: immediate (from burst)
    // Second request: immediate (from burst)
    // Third request: delayed (~50ms to refill 1 MB at 20 MB/s)
    for (int i = 0; i < 3; i++) {
        int ret = buckets_throttle_wait(g_ctx.throttle, 1024 * 1024);
        cr_assert_eq(ret, BUCKETS_OK, "Wait %d should succeed", i);
    }
    
    i64 elapsed_us = get_time_us() - start_us;
    
    // Should take at least 40ms for third request
    cr_assert_geq(elapsed_us, 40000, "Should be throttled after burst (>=40ms)");
}

/**
 * Test 12: NULL parameter validation
 */
Test(throttle, null_validation)
{
    // Wait with NULL throttle
    int ret = buckets_throttle_wait(NULL, 1024);
    cr_assert_eq(ret, BUCKETS_ERR_INVALID_ARG, "Should reject NULL throttle");
    
    // Get rate with NULL throttle
    i64 rate = buckets_throttle_get_rate(NULL);
    cr_assert_eq(rate, 0, "Should return 0 for NULL throttle");
    
    // Is enabled with NULL throttle
    bool enabled = buckets_throttle_is_enabled(NULL);
    cr_assert(!enabled, "Should return false for NULL throttle");
}

/**
 * Test 13: Zero byte wait (no-op)
 */
Test(throttle, wait_zero_bytes)
{
    g_ctx.throttle = buckets_throttle_create(100, 10);
    
    i64 start_us = get_time_us();
    
    int ret = buckets_throttle_wait(g_ctx.throttle, 0);
    
    i64 elapsed_us = get_time_us() - start_us;
    
    cr_assert_eq(ret, BUCKETS_OK, "Should succeed with zero bytes");
    cr_assert_lt(elapsed_us, 1000, "Should be instant (<1ms)");
}

/**
 * Test 14: Burst behavior (large request from full bucket)
 */
Test(throttle, burst_behavior)
{
    // 10 MB/s rate, 5 MB burst
    g_ctx.throttle = buckets_throttle_create(10, 5);
    
    i64 start_us = get_time_us();
    
    // Request 5 MB (entire burst) - should be immediate
    int ret = buckets_throttle_wait(g_ctx.throttle, 5LL * 1024 * 1024);
    
    i64 elapsed_us = get_time_us() - start_us;
    
    cr_assert_eq(ret, BUCKETS_OK, "Should succeed");
    cr_assert_lt(elapsed_us, 10000, "Burst should be immediate (<10ms)");
    
    // Now request another 5 MB - should be throttled
    start_us = get_time_us();
    ret = buckets_throttle_wait(g_ctx.throttle, 5LL * 1024 * 1024);
    elapsed_us = get_time_us() - start_us;
    
    cr_assert_eq(ret, BUCKETS_OK, "Should succeed");
    
    // Should take ~500ms at 10 MB/s to refill 5 MB
    cr_assert_geq(elapsed_us, 450000, "Should be throttled (>=450ms)");
}

/**
 * Test 15: Set rate to zero (disable)
 */
Test(throttle, set_rate_zero)
{
    g_ctx.throttle = buckets_throttle_create(100, 10);
    
    cr_assert(buckets_throttle_is_enabled(g_ctx.throttle), "Should start enabled");
    
    // Set rate to zero (disables throttling)
    int ret = buckets_throttle_set_rate(g_ctx.throttle, 0);
    cr_assert_eq(ret, BUCKETS_OK, "Should set rate to zero");
    
    cr_assert(!buckets_throttle_is_enabled(g_ctx.throttle), "Should be disabled");
    
    // Large request should be instant
    i64 start_us = get_time_us();
    ret = buckets_throttle_wait(g_ctx.throttle, 100LL * 1024 * 1024);
    i64 elapsed_us = get_time_us() - start_us;
    
    cr_assert_eq(ret, BUCKETS_OK, "Should succeed");
    cr_assert_lt(elapsed_us, 10000, "Should be instant when disabled (<10ms)");
}
