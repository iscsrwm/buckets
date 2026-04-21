/**
 * HTTP Worker Pool API
 * 
 * Multi-process worker pool for scaling HTTP server across CPU cores.
 * Named buckets_http_worker_* to avoid conflicts with migration worker pool.
 * 
 * Usage:
 *   1. Define a callback that starts your server
 *   2. Call buckets_http_worker_start() with desired worker count
 *   3. Call buckets_http_worker_run() to enter master loop
 *   4. Master monitors workers and handles graceful shutdown
 */

#ifndef BUCKETS_WORKER_POOL_H
#define BUCKETS_WORKER_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HTTP worker callback function type
 * 
 * Called in each forked worker process. Should create and run the server.
 * 
 * @param worker_id Worker number (0-based)
 * @param user_data User data passed from buckets_http_worker_start()
 * @return Exit code (0 for success)
 */
typedef int (*buckets_http_worker_callback_t)(int worker_id, void *user_data);

/**
 * Start HTTP worker pool with specified number of workers
 * 
 * Forks N worker processes, each running the callback function.
 * Returns immediately in parent (master) process.
 * Never returns in child (worker) processes.
 * 
 * @param num_workers Number of worker processes (1-64)
 * @param callback Function to run in each worker
 * @param user_data User data passed to callback
 * @return 0 on success, -1 on error
 */
int buckets_http_worker_start(int num_workers, buckets_http_worker_callback_t callback, void *user_data);

/**
 * Run master process main loop
 * 
 * Monitors workers and handles graceful shutdown.
 * Blocks until shutdown requested (SIGINT/SIGTERM).
 * 
 * @return 0 on success, -1 on error
 */
int buckets_http_worker_run(void);

/**
 * Get optimal number of workers based on CPU cores
 * 
 * @return Recommended number of workers
 */
int buckets_http_worker_get_optimal_count(void);

/**
 * Get worker pool statistics
 * 
 * @param total Output: total number of workers
 * @param alive Output: number of alive workers
 * @param dead Output: number of dead workers
 */
void buckets_http_worker_stats(int *total, int *alive, int *dead);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_WORKER_POOL_H */
