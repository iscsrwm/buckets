/**
 * UV Server Performance Metrics
 * 
 * Tracks detailed performance metrics to diagnose multi-client bottlenecks.
 */

#ifndef UV_SERVER_METRICS_H
#define UV_SERVER_METRICS_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>

/* Enable metrics collection */
#define UV_SERVER_METRICS_ENABLED 1

typedef struct {
    /* Connection metrics */
    uint64_t total_connections;
    uint64_t active_connections;
    uint64_t rejected_connections;  /* Hit max_connections limit */
    
    /* Request metrics */
    uint64_t total_requests;
    uint64_t active_requests;       /* Currently processing */
    uint64_t async_requests;        /* In thread pool */
    
    /* Timing histograms (microseconds) */
    uint64_t request_latency_min;
    uint64_t request_latency_max;
    uint64_t request_latency_sum;
    uint64_t request_latency_count;
    
    /* Thread pool metrics */
    uint64_t threadpool_queue_depth;  /* Estimated async work queued */
    uint64_t threadpool_wait_time_sum;  /* Time waiting for thread pool */
    uint64_t threadpool_wait_count;
    
    /* Lock contention metrics */
    uint64_t write_lock_wait_time_sum;  /* Time waiting for write_lock */
    uint64_t write_lock_wait_count;
    
    /* Error counters */
    uint64_t timeout_errors;
    uint64_t parse_errors;
    uint64_t write_errors;
    
    /* Last snapshot time */
    struct timespec last_snapshot;
    
    /* Mutex for thread-safe updates */
    pthread_mutex_t lock;
} uv_server_metrics_t;

/* Global metrics instance */
extern uv_server_metrics_t g_uv_metrics;

/* Initialize metrics */
void uv_metrics_init(void);

/* Connection tracking */
void uv_metrics_conn_accepted(void);
void uv_metrics_conn_rejected(void);
void uv_metrics_conn_closed(void);

/* Request tracking */
void uv_metrics_request_start(void);
void uv_metrics_request_end(uint64_t latency_us);
void uv_metrics_async_start(void);
void uv_metrics_async_end(uint64_t wait_time_us);

/* Lock contention tracking */
void uv_metrics_write_lock_wait(uint64_t wait_time_us);

/* Error tracking */
void uv_metrics_timeout(void);
void uv_metrics_parse_error(void);
void uv_metrics_write_error(void);

/* Snapshot and print */
void uv_metrics_snapshot(void);
void uv_metrics_print(void);

/* Timing helper - returns microseconds since epoch */
static inline uint64_t uv_metrics_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

#endif /* UV_SERVER_METRICS_H */
