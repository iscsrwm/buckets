/**
 * Migration Throttle Implementation
 * 
 * Bandwidth and I/O throttling for background migration.
 * 
 * Components:
 * 1. Token bucket algorithm for bandwidth limiting
 * 2. I/O prioritization (user > migration)
 * 3. Configurable rate limits (MB/s, IOPS)
 * 4. Dynamic adjustment based on load
 * 
 * Token Bucket Algorithm:
 * - Tokens represent bytes that can be transferred
 * - Bucket refills at configured rate (bytes/sec)
 * - Workers consume tokens before each I/O operation
 * - If insufficient tokens, worker sleeps until refill
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "buckets.h"
#include "buckets_migration.h"

/* ===================================================================
 * Token Bucket Implementation
 * ===================================================================*/

/**
 * Initialize token bucket
 */
int buckets_throttle_init(buckets_throttle_t *throttle,
                           i64 rate_bytes_per_sec,
                           i64 burst_bytes)
{
    if (!throttle || rate_bytes_per_sec < 0 || burst_bytes < 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    throttle->rate_bytes_per_sec = rate_bytes_per_sec;
    throttle->burst_bytes = burst_bytes;
    throttle->tokens = burst_bytes;  // Start with full bucket
    
    // Initialize timestamp
    struct timeval tv;
    gettimeofday(&tv, NULL);
    throttle->last_refill_us = (i64)tv.tv_sec * 1000000LL + tv.tv_usec;
    
    pthread_mutex_init(&throttle->lock, NULL);
    
    throttle->enabled = (rate_bytes_per_sec > 0);
    
    buckets_debug("Throttle initialized: rate=%lld B/s, burst=%lld B, enabled=%d",
                  (long long)rate_bytes_per_sec, 
                  (long long)burst_bytes,
                  throttle->enabled);
    
    return BUCKETS_OK;
}

/**
 * Cleanup throttle
 */
void buckets_throttle_cleanup(buckets_throttle_t *throttle)
{
    if (!throttle) {
        return;
    }
    
    pthread_mutex_destroy(&throttle->lock);
}

/**
 * Get current time in microseconds
 */
static i64 get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (i64)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/**
 * Refill token bucket based on elapsed time
 */
static void refill_tokens(buckets_throttle_t *throttle, i64 now_us)
{
    i64 elapsed_us = now_us - throttle->last_refill_us;
    
    if (elapsed_us <= 0) {
        return;  // No time elapsed
    }
    
    // Calculate tokens to add: (elapsed_us * rate) / 1,000,000
    i64 tokens_to_add = (elapsed_us * throttle->rate_bytes_per_sec) / 1000000LL;
    
    if (tokens_to_add > 0) {
        throttle->tokens += tokens_to_add;
        
        // Cap at burst size
        if (throttle->tokens > throttle->burst_bytes) {
            throttle->tokens = throttle->burst_bytes;
        }
        
        throttle->last_refill_us = now_us;
    }
}

/**
 * Wait for tokens to become available and consume them
 */
int buckets_throttle_wait(buckets_throttle_t *throttle, i64 bytes)
{
    if (!throttle) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    // If throttling disabled, return immediately
    if (!throttle->enabled || throttle->rate_bytes_per_sec == 0) {
        return BUCKETS_OK;
    }
    
    if (bytes <= 0) {
        return BUCKETS_OK;  // Nothing to throttle
    }
    
    pthread_mutex_lock(&throttle->lock);
    
    while (true) {
        i64 now_us = get_time_us();
        
        // Refill bucket
        refill_tokens(throttle, now_us);
        
        // Check if enough tokens available
        if (throttle->tokens >= bytes) {
            // Consume tokens
            throttle->tokens -= bytes;
            pthread_mutex_unlock(&throttle->lock);
            return BUCKETS_OK;
        }
        
        // Not enough tokens - calculate sleep time
        i64 tokens_needed = bytes - throttle->tokens;
        i64 sleep_us = (tokens_needed * 1000000LL) / throttle->rate_bytes_per_sec;
        
        // Cap sleep at 100ms to allow for periodic refill checks
        if (sleep_us > 100000) {
            sleep_us = 100000;
        }
        
        pthread_mutex_unlock(&throttle->lock);
        
        // Sleep and retry
        struct timespec ts;
        ts.tv_sec = sleep_us / 1000000LL;
        ts.tv_nsec = (sleep_us % 1000000LL) * 1000LL;
        nanosleep(&ts, NULL);
        
        pthread_mutex_lock(&throttle->lock);
    }
}

/**
 * Set throttle rate (can be changed dynamically)
 */
int buckets_throttle_set_rate(buckets_throttle_t *throttle, i64 rate_bytes_per_sec)
{
    if (!throttle || rate_bytes_per_sec < 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&throttle->lock);
    
    throttle->rate_bytes_per_sec = rate_bytes_per_sec;
    throttle->enabled = (rate_bytes_per_sec > 0);
    
    buckets_info("Throttle rate updated: %lld B/s (%s)",
                 (long long)rate_bytes_per_sec,
                 throttle->enabled ? "enabled" : "disabled");
    
    pthread_mutex_unlock(&throttle->lock);
    
    return BUCKETS_OK;
}

/**
 * Get current throttle rate
 */
i64 buckets_throttle_get_rate(buckets_throttle_t *throttle)
{
    if (!throttle) {
        return 0;
    }
    
    pthread_mutex_lock(&throttle->lock);
    i64 rate = throttle->rate_bytes_per_sec;
    pthread_mutex_unlock(&throttle->lock);
    
    return rate;
}

/**
 * Enable/disable throttling
 */
int buckets_throttle_set_enabled(buckets_throttle_t *throttle, bool enabled)
{
    if (!throttle) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&throttle->lock);
    throttle->enabled = enabled;
    pthread_mutex_unlock(&throttle->lock);
    
    buckets_info("Throttle %s", enabled ? "enabled" : "disabled");
    
    return BUCKETS_OK;
}

/**
 * Check if throttling is enabled
 */
bool buckets_throttle_is_enabled(buckets_throttle_t *throttle)
{
    if (!throttle) {
        return false;
    }
    
    pthread_mutex_lock(&throttle->lock);
    bool enabled = throttle->enabled;
    pthread_mutex_unlock(&throttle->lock);
    
    return enabled;
}

/**
 * Get throttle statistics
 */
int buckets_throttle_get_stats(buckets_throttle_t *throttle,
                                 i64 *current_tokens,
                                 i64 *rate_bytes_per_sec,
                                 bool *enabled)
{
    if (!throttle) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&throttle->lock);
    
    // Refill tokens to get current state
    i64 now_us = get_time_us();
    refill_tokens(throttle, now_us);
    
    if (current_tokens) *current_tokens = throttle->tokens;
    if (rate_bytes_per_sec) *rate_bytes_per_sec = throttle->rate_bytes_per_sec;
    if (enabled) *enabled = throttle->enabled;
    
    pthread_mutex_unlock(&throttle->lock);
    
    return BUCKETS_OK;
}

/**
 * Create throttle with default settings
 */
buckets_throttle_t* buckets_throttle_create_default(void)
{
    buckets_throttle_t *throttle = buckets_calloc(1, sizeof(buckets_throttle_t));
    if (!throttle) {
        return NULL;
    }
    
    // Default: 100 MB/s rate, 10 MB burst
    i64 rate = 100LL * 1024 * 1024;   // 100 MB/s
    i64 burst = 10LL * 1024 * 1024;   // 10 MB burst
    
    int ret = buckets_throttle_init(throttle, rate, burst);
    if (ret != BUCKETS_OK) {
        buckets_free(throttle);
        return NULL;
    }
    
    return throttle;
}

/**
 * Create throttle with custom settings
 */
buckets_throttle_t* buckets_throttle_create(i64 rate_mbps, i64 burst_mb)
{
    if (rate_mbps < 0 || burst_mb < 0) {
        return NULL;
    }
    
    buckets_throttle_t *throttle = buckets_calloc(1, sizeof(buckets_throttle_t));
    if (!throttle) {
        return NULL;
    }
    
    // Convert MB/s to bytes/s
    i64 rate_bytes = rate_mbps * 1024 * 1024;
    i64 burst_bytes = burst_mb * 1024 * 1024;
    
    int ret = buckets_throttle_init(throttle, rate_bytes, burst_bytes);
    if (ret != BUCKETS_OK) {
        buckets_free(throttle);
        return NULL;
    }
    
    return throttle;
}

/**
 * Free throttle
 */
void buckets_throttle_free(buckets_throttle_t *throttle)
{
    if (!throttle) {
        return;
    }
    
    buckets_throttle_cleanup(throttle);
    buckets_free(throttle);
}
