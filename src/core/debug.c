/**
 * Debug Instrumentation Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <pthread.h>

#include "buckets.h"
#include "buckets_debug.h"

/* ===================================================================
 * Global State
 * ===================================================================*/

bool g_debug_instrumentation_enabled = false;

buckets_debug_stats_t g_stats;
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

static _Atomic uint64_t g_request_counter = 0;

/* ===================================================================
 * Initialization
 * ===================================================================*/

void buckets_debug_init(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    g_debug_instrumentation_enabled = false;
    buckets_info("Debug instrumentation initialized (disabled by default)");
}

void buckets_debug_cleanup(void)
{
    g_debug_instrumentation_enabled = false;
}

void buckets_debug_set_enabled(bool enabled)
{
    g_debug_instrumentation_enabled = enabled;
    if (enabled) {
        buckets_info("Debug instrumentation ENABLED");
    } else {
        buckets_info("Debug instrumentation DISABLED");
    }
}

/* ===================================================================
 * Statistics
 * ===================================================================*/

void buckets_debug_get_stats(buckets_debug_stats_t *stats)
{
    if (!stats) return;
    
    pthread_mutex_lock(&g_stats_lock);
    memcpy(stats, &g_stats, sizeof(buckets_debug_stats_t));
    pthread_mutex_unlock(&g_stats_lock);
}

void buckets_debug_reset_stats(void)
{
    pthread_mutex_lock(&g_stats_lock);
    memset(&g_stats, 0, sizeof(g_stats));
    pthread_mutex_unlock(&g_stats_lock);
    buckets_info("Debug statistics reset");
}

void buckets_debug_print_stats(void)
{
    buckets_debug_stats_t stats;
    buckets_debug_get_stats(&stats);
    
    buckets_info("=== Debug Statistics ===");
    buckets_info("RPC: total=%lu active=%lu failed=%lu timeouts=%lu sem_waits=%lu",
                 stats.rpc_calls_total, stats.rpc_calls_active, stats.rpc_calls_failed,
                 stats.rpc_timeouts, stats.rpc_semaphore_waits);
    buckets_info("Connections: total=%lu active=%lu failed=%lu timeouts=%lu",
                 stats.conn_pool_total, stats.conn_pool_active, 
                 stats.conn_pool_failures, stats.conn_pool_timeouts);
    buckets_info("Binary Transport Writes: total=%lu active=%lu failed=%lu",
                 stats.binary_writes_total, stats.binary_writes_active, 
                 stats.binary_writes_failed);
    buckets_info("Binary Transport Reads: total=%lu active=%lu failed=%lu",
                 stats.binary_reads_total, stats.binary_reads_active, 
                 stats.binary_reads_failed);
    buckets_info("Erasure: encode_total=%lu encode_active=%lu decode_total=%lu decode_active=%lu",
                 stats.erasure_encodes_total, stats.erasure_encodes_active,
                 stats.erasure_decodes_total, stats.erasure_decodes_active);
    buckets_info("Disk I/O Writes: total=%lu active=%lu failed=%lu",
                 stats.disk_writes_total, stats.disk_writes_active, 
                 stats.disk_writes_failed);
    buckets_info("Disk I/O Reads: total=%lu active=%lu failed=%lu",
                 stats.disk_reads_total, stats.disk_reads_active, 
                 stats.disk_reads_failed);
    buckets_info("Requests: total=%lu active=%lu failed=%lu",
                 stats.requests_total, stats.requests_active, stats.requests_failed);
}

void buckets_debug_inc(uint64_t *counter)
{
    if (!g_debug_instrumentation_enabled) return;
    
    pthread_mutex_lock(&g_stats_lock);
    (*counter)++;
    pthread_mutex_unlock(&g_stats_lock);
}

void buckets_debug_dec(uint64_t *counter)
{
    if (!g_debug_instrumentation_enabled) return;
    
    pthread_mutex_lock(&g_stats_lock);
    if (*counter > 0) (*counter)--;
    pthread_mutex_unlock(&g_stats_lock);
}

/* ===================================================================
 * Request Timing
 * ===================================================================*/

static double timespec_diff_ms(struct timespec *start, struct timespec *end)
{
    double diff = (end->tv_sec - start->tv_sec) * 1000.0;
    diff += (end->tv_nsec - start->tv_nsec) / 1000000.0;
    return diff;
}

buckets_request_timing_t* buckets_debug_timing_start(const char *operation,
                                                       const char *bucket,
                                                       const char *object)
{
    if (!g_debug_instrumentation_enabled) return NULL;
    
    buckets_request_timing_t *timing = buckets_calloc(1, sizeof(buckets_request_timing_t));
    if (!timing) return NULL;
    
    clock_gettime(CLOCK_MONOTONIC, &timing->start_time);
    timing->request_id = atomic_fetch_add(&g_request_counter, 1);
    
    snprintf(timing->operation, sizeof(timing->operation), "%s", operation ? operation : "unknown");
    snprintf(timing->bucket, sizeof(timing->bucket), "%s", bucket ? bucket : "");
    snprintf(timing->object, sizeof(timing->object), "%s", object ? object : "");
    
    buckets_debug("[REQ %lu] START %s %s/%s", 
                  timing->request_id, timing->operation, 
                  timing->bucket, timing->object);
    
    return timing;
}

void buckets_debug_timing_mark(buckets_request_timing_t *timing, const char *phase)
{
    if (!g_debug_instrumentation_enabled || !timing) return;
    
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    double elapsed = timespec_diff_ms(&timing->start_time, &now);
    
    if (strcmp(phase, "parse") == 0) {
        timing->parse_time = now;
    } else if (strcmp(phase, "auth") == 0) {
        timing->auth_time = now;
    } else if (strcmp(phase, "storage_start") == 0) {
        timing->storage_start_time = now;
    } else if (strcmp(phase, "storage_end") == 0) {
        timing->storage_end_time = now;
    } else if (strcmp(phase, "erasure_start") == 0) {
        timing->erasure_start_time = now;
    } else if (strcmp(phase, "erasure_end") == 0) {
        timing->erasure_end_time = now;
    } else if (strcmp(phase, "rpc_start") == 0) {
        timing->rpc_start_time = now;
    } else if (strcmp(phase, "rpc_end") == 0) {
        timing->rpc_end_time = now;
    }
    
    buckets_debug("[REQ %lu] MARK %s at %.2f ms", 
                  timing->request_id, phase, elapsed);
}

void buckets_debug_timing_end(buckets_request_timing_t *timing)
{
    if (!g_debug_instrumentation_enabled || !timing) return;
    
    clock_gettime(CLOCK_MONOTONIC, &timing->end_time);
    
    double total = timespec_diff_ms(&timing->start_time, &timing->end_time);
    double storage_time = 0;
    double erasure_time = 0;
    double rpc_time = 0;
    
    if (timing->storage_start_time.tv_sec > 0 && timing->storage_end_time.tv_sec > 0) {
        storage_time = timespec_diff_ms(&timing->storage_start_time, &timing->storage_end_time);
    }
    
    if (timing->erasure_start_time.tv_sec > 0 && timing->erasure_end_time.tv_sec > 0) {
        erasure_time = timespec_diff_ms(&timing->erasure_start_time, &timing->erasure_end_time);
    }
    
    if (timing->rpc_start_time.tv_sec > 0 && timing->rpc_end_time.tv_sec > 0) {
        rpc_time = timespec_diff_ms(&timing->rpc_start_time, &timing->rpc_end_time);
    }
    
    buckets_info("[REQ %lu] END %s %s/%s - total=%.2f ms storage=%.2f ms erasure=%.2f ms rpc=%.2f ms",
                 timing->request_id, timing->operation, timing->bucket, timing->object,
                 total, storage_time, erasure_time, rpc_time);
    
    buckets_free(timing);
}
