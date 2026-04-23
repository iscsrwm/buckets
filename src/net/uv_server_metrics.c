/**
 * UV Server Performance Metrics Implementation
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "buckets.h"
#include "uv_server_metrics.h"

/* Global metrics */
uv_server_metrics_t g_uv_metrics;

void uv_metrics_init(void) {
    memset(&g_uv_metrics, 0, sizeof(g_uv_metrics));
    pthread_mutex_init(&g_uv_metrics.lock, NULL);
    clock_gettime(CLOCK_MONOTONIC, &g_uv_metrics.last_snapshot);
    g_uv_metrics.request_latency_min = UINT64_MAX;
}

void uv_metrics_conn_accepted(void) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    g_uv_metrics.total_connections++;
    g_uv_metrics.active_connections++;
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_conn_rejected(void) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    g_uv_metrics.rejected_connections++;
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_conn_closed(void) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    if (g_uv_metrics.active_connections > 0) {
        g_uv_metrics.active_connections--;
    }
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_request_start(void) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    g_uv_metrics.total_requests++;
    g_uv_metrics.active_requests++;
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_request_end(uint64_t latency_us) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    if (g_uv_metrics.active_requests > 0) {
        g_uv_metrics.active_requests--;
    }
    
    /* Update latency histogram */
    if (latency_us < g_uv_metrics.request_latency_min) {
        g_uv_metrics.request_latency_min = latency_us;
    }
    if (latency_us > g_uv_metrics.request_latency_max) {
        g_uv_metrics.request_latency_max = latency_us;
    }
    g_uv_metrics.request_latency_sum += latency_us;
    g_uv_metrics.request_latency_count++;
    
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_async_start(void) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    g_uv_metrics.async_requests++;
    g_uv_metrics.threadpool_queue_depth++;
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_async_end(uint64_t wait_time_us) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    if (g_uv_metrics.async_requests > 0) {
        g_uv_metrics.async_requests--;
    }
    if (g_uv_metrics.threadpool_queue_depth > 0) {
        g_uv_metrics.threadpool_queue_depth--;
    }
    g_uv_metrics.threadpool_wait_time_sum += wait_time_us;
    g_uv_metrics.threadpool_wait_count++;
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_write_lock_wait(uint64_t wait_time_us) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    g_uv_metrics.write_lock_wait_time_sum += wait_time_us;
    g_uv_metrics.write_lock_wait_count++;
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_timeout(void) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    g_uv_metrics.timeout_errors++;
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_parse_error(void) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    g_uv_metrics.parse_errors++;
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_write_error(void) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    g_uv_metrics.write_errors++;
    pthread_mutex_unlock(&g_uv_metrics.lock);
}

void uv_metrics_snapshot(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    
    pthread_mutex_lock(&g_uv_metrics.lock);
    
    /* Calculate time since last snapshot */
    double elapsed = (now.tv_sec - g_uv_metrics.last_snapshot.tv_sec) +
                     (now.tv_nsec - g_uv_metrics.last_snapshot.tv_nsec) / 1e9;
    
    if (elapsed < 10.0) {
        pthread_mutex_unlock(&g_uv_metrics.lock);
        return;  /* Only print every 10 seconds */
    }
    
    g_uv_metrics.last_snapshot = now;
    pthread_mutex_unlock(&g_uv_metrics.lock);
    
    uv_metrics_print();
}

void uv_metrics_print(void) {
    pthread_mutex_lock(&g_uv_metrics.lock);
    
    buckets_info("=== UV SERVER METRICS ===");
    buckets_info("Connections: %lu total, %lu active, %lu rejected",
                 g_uv_metrics.total_connections,
                 g_uv_metrics.active_connections,
                 g_uv_metrics.rejected_connections);
    
    buckets_info("Requests: %lu total, %lu active, %lu async",
                 g_uv_metrics.total_requests,
                 g_uv_metrics.active_requests,
                 g_uv_metrics.async_requests);
    
    if (g_uv_metrics.request_latency_count > 0) {
        uint64_t avg_latency = g_uv_metrics.request_latency_sum / 
                                g_uv_metrics.request_latency_count;
        buckets_info("Latency: min=%lu us, max=%lu us, avg=%lu us",
                     g_uv_metrics.request_latency_min,
                     g_uv_metrics.request_latency_max,
                     avg_latency);
    }
    
    buckets_info("Thread Pool: queue_depth=%lu",
                 g_uv_metrics.threadpool_queue_depth);
    
    if (g_uv_metrics.threadpool_wait_count > 0) {
        uint64_t avg_wait = g_uv_metrics.threadpool_wait_time_sum /
                            g_uv_metrics.threadpool_wait_count;
        buckets_info("Thread Pool Wait: avg=%lu us (%lu samples)",
                     avg_wait, g_uv_metrics.threadpool_wait_count);
    }
    
    if (g_uv_metrics.write_lock_wait_count > 0) {
        uint64_t avg_wait = g_uv_metrics.write_lock_wait_time_sum /
                            g_uv_metrics.write_lock_wait_count;
        buckets_info("Write Lock Wait: avg=%lu us (%lu waits)",
                     avg_wait, g_uv_metrics.write_lock_wait_count);
    }
    
    buckets_info("Errors: timeouts=%lu, parse=%lu, write=%lu",
                 g_uv_metrics.timeout_errors,
                 g_uv_metrics.parse_errors,
                 g_uv_metrics.write_errors);
    
    buckets_info("=========================");
    
    pthread_mutex_unlock(&g_uv_metrics.lock);
}
