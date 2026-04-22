/**
 * Debug Instrumentation
 * 
 * Comprehensive debug tracking for multi-client coordination issues.
 */

#ifndef BUCKETS_DEBUG_H
#define BUCKETS_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ===================================================================
 * Configuration
 * ===================================================================*/

/* Enable/disable debug instrumentation (can be toggled at runtime) */
extern bool g_debug_instrumentation_enabled;

/* Statistics tracking */
typedef struct {
    /* RPC stats */
    uint64_t rpc_calls_total;
    uint64_t rpc_calls_active;
    uint64_t rpc_calls_failed;
    uint64_t rpc_timeouts;
    uint64_t rpc_semaphore_waits;
    
    /* Connection pool stats */
    uint64_t conn_pool_total;
    uint64_t conn_pool_active;
    uint64_t conn_pool_failures;
    uint64_t conn_pool_timeouts;
    
    /* Binary transport stats */
    uint64_t binary_writes_total;
    uint64_t binary_writes_active;
    uint64_t binary_writes_failed;
    uint64_t binary_reads_total;
    uint64_t binary_reads_active;
    uint64_t binary_reads_failed;
    
    /* Erasure coding stats */
    uint64_t erasure_encodes_total;
    uint64_t erasure_encodes_active;
    uint64_t erasure_decodes_total;
    uint64_t erasure_decodes_active;
    
    /* Storage stats */
    uint64_t disk_writes_total;
    uint64_t disk_writes_active;
    uint64_t disk_writes_failed;
    uint64_t disk_reads_total;
    uint64_t disk_reads_active;
    uint64_t disk_reads_failed;
    
    /* Request stats */
    uint64_t requests_total;
    uint64_t requests_active;
    uint64_t requests_failed;
} buckets_debug_stats_t;

/* Global statistics (exported for direct access by instrumented code) */
extern buckets_debug_stats_t g_stats;

/* Timing information for a single request */
typedef struct {
    struct timespec start_time;
    struct timespec parse_time;
    struct timespec auth_time;
    struct timespec storage_start_time;
    struct timespec storage_end_time;
    struct timespec erasure_start_time;
    struct timespec erasure_end_time;
    struct timespec rpc_start_time;
    struct timespec rpc_end_time;
    struct timespec end_time;
    
    uint64_t request_id;
    char operation[32];
    char bucket[256];
    char object[1024];
} buckets_request_timing_t;

/* ===================================================================
 * API
 * ===================================================================*/

/**
 * Initialize debug instrumentation
 */
void buckets_debug_init(void);

/**
 * Cleanup debug instrumentation
 */
void buckets_debug_cleanup(void);

/**
 * Enable/disable instrumentation
 */
void buckets_debug_set_enabled(bool enabled);

/**
 * Get current statistics
 */
void buckets_debug_get_stats(buckets_debug_stats_t *stats);

/**
 * Reset statistics
 */
void buckets_debug_reset_stats(void);

/**
 * Print statistics to log
 */
void buckets_debug_print_stats(void);

/**
 * Increment counter (thread-safe atomic)
 */
void buckets_debug_inc(uint64_t *counter);
void buckets_debug_dec(uint64_t *counter);

/**
 * Request timing helpers
 */
buckets_request_timing_t* buckets_debug_timing_start(const char *operation, 
                                                       const char *bucket,
                                                       const char *object);
void buckets_debug_timing_mark(buckets_request_timing_t *timing, const char *phase);
void buckets_debug_timing_end(buckets_request_timing_t *timing);

/**
 * Log a timing event (if enabled)
 */
#define DEBUG_TIMING_START(op, bucket, obj) \
    (g_debug_instrumentation_enabled ? buckets_debug_timing_start(op, bucket, obj) : NULL)

#define DEBUG_TIMING_MARK(timing, phase) \
    do { if (g_debug_instrumentation_enabled && timing) buckets_debug_timing_mark(timing, phase); } while(0)

#define DEBUG_TIMING_END(timing) \
    do { if (g_debug_instrumentation_enabled && timing) buckets_debug_timing_end(timing); } while(0)

#define DEBUG_INC(counter) \
    do { if (g_debug_instrumentation_enabled) buckets_debug_inc(&counter); } while(0)

#define DEBUG_DEC(counter) \
    do { if (g_debug_instrumentation_enabled) buckets_debug_dec(&counter); } while(0)

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_DEBUG_H */
