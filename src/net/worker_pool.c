/**
 * Worker Pool Implementation
 * 
 * Multi-process worker pool for scaling HTTP server across CPU cores.
 * Each worker process runs its own event loop to maximize throughput.
 * 
 * Architecture:
 * - Master process: coordinates workers, handles signals
 * - Worker processes: each runs independent event loop
 * - SO_REUSEPORT: allows multiple processes to bind same port
 * 
 * Usage:
 *   Set BUCKETS_WORKERS environment variable to enable multi-process mode.
 *   Workers inherit all server configuration from parent process.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>

#include "buckets.h"
#include "buckets_worker_pool.h"
#include "buckets_storage.h"  /* For buckets_chunk_reinit_after_fork */

/* Maximum number of worker processes */
#define MAX_WORKERS 64

/* Worker process state */
typedef struct {
    pid_t pid;           /* Process ID (0 if slot unused) */
    int worker_id;       /* Worker number (0-based) */
    time_t started_at;   /* Start time */
    bool alive;          /* Is worker alive? */
} worker_t;

/* Worker pool state */
static struct {
    worker_t workers[MAX_WORKERS];
    int num_workers;
    bool shutdown_requested;
    buckets_http_worker_callback_t callback;
    void *callback_data;
} g_pool = {0};

/* ===================================================================
 * Signal Handlers
 * ===================================================================*/

/**
 * Signal handler for master process
 */
static void master_signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        buckets_info("Received signal %d, initiating graceful shutdown", signo);
        g_pool.shutdown_requested = true;
    } else if (signo == SIGCHLD) {
        /* Child process died - will be handled in main loop */
    }
}

/**
 * Signal handler for worker process
 */
static void worker_signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        buckets_info("Worker received signal %d, shutting down", signo);
        exit(0);
    }
}

/* ===================================================================
 * Worker Process Management
 * ===================================================================*/

/**
 * Worker process main function
 */
static int worker_main(int worker_id, buckets_http_worker_callback_t callback, void *user_data)
{
    /* Install worker signal handlers */
    signal(SIGINT, worker_signal_handler);
    signal(SIGTERM, worker_signal_handler);
    signal(SIGCHLD, SIG_DFL);
    
    buckets_info("Worker %d started (pid=%d)", worker_id, getpid());
    
    /* Reinitialize io_uring after fork - CRITICAL for correct operation */
    buckets_chunk_reinit_after_fork();
    
    /* Set environment variable so worker knows its ID */
    char worker_id_str[32];
    snprintf(worker_id_str, sizeof(worker_id_str), "%d", worker_id);
    setenv("BUCKETS_WORKER_ID", worker_id_str, 1);
    
    /* Call user callback (runs server) */
    int ret = callback(worker_id, user_data);
    
    buckets_info("Worker %d exiting (ret=%d)", worker_id, ret);
    return ret;
}

/**
 * Spawn a worker process
 */
static pid_t spawn_worker(int worker_id, buckets_http_worker_callback_t callback, void *user_data)
{
    pid_t pid = fork();
    
    if (pid < 0) {
        /* Fork failed */
        buckets_error("Failed to fork worker %d: %s", worker_id, strerror(errno));
        return -1;
    }
    
    if (pid == 0) {
        /* Child process - become worker */
        int ret = worker_main(worker_id, callback, user_data);
        exit(ret);
    }
    
    /* Parent process - return child PID */
    return pid;
}

/**
 * Start a worker
 */
static int start_worker(int worker_id)
{
    if (worker_id >= g_pool.num_workers) {
        return -1;
    }
    
    worker_t *worker = &g_pool.workers[worker_id];
    
    pid_t pid = spawn_worker(worker_id, g_pool.callback, g_pool.callback_data);
    if (pid < 0) {
        return -1;
    }
    
    worker->pid = pid;
    worker->worker_id = worker_id;
    worker->started_at = time(NULL);
    worker->alive = true;
    
    buckets_info("Started worker %d (pid=%d)", worker_id, pid);
    return 0;
}

/**
 * Stop a worker (graceful)
 */
static void stop_worker(int worker_id)
{
    if (worker_id >= g_pool.num_workers) {
        return;
    }
    
    worker_t *worker = &g_pool.workers[worker_id];
    
    if (worker->pid > 0) {
        buckets_info("Stopping worker %d (pid=%d)", worker_id, worker->pid);
        kill(worker->pid, SIGTERM);
        worker->alive = false;
    }
}

/**
 * Reap dead workers and check status
 */
static void reap_workers(void)
{
    int status;
    pid_t pid;
    
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Find which worker died */
        for (int i = 0; i < g_pool.num_workers; i++) {
            if (g_pool.workers[i].pid == pid) {
                if (WIFEXITED(status)) {
                    int exit_code = WEXITSTATUS(status);
                    buckets_info("Worker %d (pid=%d) exited with code %d",
                                i, pid, exit_code);
                } else if (WIFSIGNALED(status)) {
                    int signal = WTERMSIG(status);
                    buckets_warn("Worker %d (pid=%d) killed by signal %d",
                                i, pid, signal);
                }
                
                g_pool.workers[i].alive = false;
                g_pool.workers[i].pid = 0;
                
                /* Restart worker if not shutting down */
                if (!g_pool.shutdown_requested) {
                    buckets_info("Restarting worker %d", i);
                    start_worker(i);
                }
                
                break;
            }
        }
    }
}

/* ===================================================================
 * Public API
 * ===================================================================*/

/**
 * Initialize worker pool and start all workers
 * 
 * @param num_workers Number of worker processes to spawn
 * @param callback Function to run in each worker (should start server)
 * @param user_data User data passed to callback
 * @return 0 on success, -1 on error
 */
int buckets_http_worker_start(int num_workers, buckets_http_worker_callback_t callback, void *user_data)
{
    if (!callback) {
        buckets_error("NULL callback in worker_pool_start");
        return -1;
    }
    
    if (num_workers <= 0 || num_workers > MAX_WORKERS) {
        buckets_error("Invalid num_workers: %d (must be 1-%d)", num_workers, MAX_WORKERS);
        return -1;
    }
    
    /* Initialize pool state */
    memset(&g_pool, 0, sizeof(g_pool));
    g_pool.num_workers = num_workers;
    g_pool.callback = callback;
    g_pool.callback_data = user_data;
    g_pool.shutdown_requested = false;
    
    /* Install master signal handlers */
    signal(SIGINT, master_signal_handler);
    signal(SIGTERM, master_signal_handler);
    signal(SIGCHLD, master_signal_handler);
    
    buckets_info("Starting worker pool with %d workers", num_workers);
    
    /* Start all workers */
    for (int i = 0; i < num_workers; i++) {
        if (start_worker(i) != 0) {
            buckets_error("Failed to start worker %d", i);
            /* Stop already started workers */
            for (int j = 0; j < i; j++) {
                stop_worker(j);
            }
            return -1;
        }
    }
    
    buckets_info("All %d workers started successfully", num_workers);
    return 0;
}

/**
 * Master process main loop
 * Monitors workers and handles restarts
 */
int buckets_http_worker_run(void)
{
    buckets_info("Master process running (pid=%d)", getpid());
    
    /* Main loop: monitor workers */
    while (!g_pool.shutdown_requested) {
        /* Reap any dead workers */
        reap_workers();
        
        /* Sleep before next check */
        sleep(1);
    }
    
    buckets_info("Shutdown requested, stopping all workers");
    
    /* Stop all workers gracefully */
    for (int i = 0; i < g_pool.num_workers; i++) {
        stop_worker(i);
    }
    
    /* Wait for all workers to exit (with timeout) */
    int timeout = 10; /* seconds */
    int remaining = g_pool.num_workers;
    
    for (int t = 0; t < timeout && remaining > 0; t++) {
        sleep(1);
        
        /* Count remaining workers */
        remaining = 0;
        for (int i = 0; i < g_pool.num_workers; i++) {
            if (g_pool.workers[i].pid > 0 && g_pool.workers[i].alive) {
                remaining++;
            }
        }
        
        /* Reap workers */
        reap_workers();
    }
    
    /* Force kill any remaining workers */
    if (remaining > 0) {
        buckets_warn("Forcing shutdown of %d remaining workers", remaining);
        for (int i = 0; i < g_pool.num_workers; i++) {
            if (g_pool.workers[i].pid > 0) {
                kill(g_pool.workers[i].pid, SIGKILL);
                waitpid(g_pool.workers[i].pid, NULL, 0);
            }
        }
    }
    
    buckets_info("Worker pool shutdown complete");
    return 0;
}

/**
 * Get optimal number of workers (based on CPU cores)
 */
int buckets_http_worker_get_optimal_count(void)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu <= 0) {
        buckets_warn("Failed to detect CPU count, defaulting to 4 workers");
        return 4;
    }
    
    /* Use all available cores */
    int workers = (int)ncpu;
    if (workers > MAX_WORKERS) {
        workers = MAX_WORKERS;
    }
    
    buckets_info("Detected %ld CPU cores, recommending %d workers", ncpu, workers);
    return workers;
}

/**
 * Get current worker pool statistics
 */
void buckets_http_worker_stats(int *total, int *alive, int *dead)
{
    int alive_count = 0;
    int dead_count = 0;
    
    for (int i = 0; i < g_pool.num_workers; i++) {
        if (g_pool.workers[i].pid > 0 && g_pool.workers[i].alive) {
            alive_count++;
        } else if (g_pool.workers[i].pid > 0) {
            dead_count++;
        }
    }
    
    if (total) *total = g_pool.num_workers;
    if (alive) *alive = alive_count;
    if (dead) *dead = dead_count;
}
