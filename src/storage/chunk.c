/**
 * Chunk I/O Implementation
 * 
 * Chunk read/write operations with checksum verification.
 * Now with io_uring async I/O for high performance.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <libgen.h>
#include <errno.h>
#include <pthread.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_crypto.h"
#include "buckets_io.h"
#include "buckets_group_commit.h"
#include "buckets_io_uring.h"

/* ===================================================================
 * io_uring Context (for async I/O)
 * ===================================================================*/

static buckets_io_uring_context_t *g_io_uring_ctx = NULL;
static pthread_once_t g_io_uring_once = PTHREAD_ONCE_INIT;

static void init_io_uring_ctx(void)
{
    buckets_info("init_io_uring_ctx called (pid=%d)", getpid());
    
    buckets_io_uring_config_t config = {
        .queue_depth = 1024,     /* Increased from 512 for better concurrency */
        .batch_size = 128,       /* Increased from 64 to match queue depth */
        .sq_poll = true,         /* ENABLED: Kernel polling for zero-syscall submission */
        .io_poll = false         /* Keep disabled (not needed for block devices) */
    };
    
    g_io_uring_ctx = buckets_io_uring_init(&config);
    if (!g_io_uring_ctx) {
        buckets_warn("Failed to initialize io_uring, falling back to blocking I/O");
    } else {
        buckets_info("✓ io_uring initialized for async chunk I/O (queue_depth=1024, sq_poll=ON, ctx=%p, pid=%d)", 
                     g_io_uring_ctx, getpid());
    }
}

buckets_io_uring_context_t* buckets_chunk_get_io_uring_ctx(void)
{
    pthread_once(&g_io_uring_once, init_io_uring_ctx);
    return g_io_uring_ctx;
}

/**
 * Reinitialize io_uring after fork()
 * 
 * MUST be called in child process after fork() because:
 * 1. io_uring file descriptors are not valid across fork
 * 2. pthread_once state is copied, so child thinks it's already initialized
 * 
 * This function resets the pthread_once state and forces reinitialization.
 */
void buckets_chunk_reinit_after_fork(void)
{
    buckets_info("Reinitializing io_uring after fork (pid=%d)", getpid());
    
    /* Reset pthread_once - this is safe because we're in a new process */
    g_io_uring_once = (pthread_once_t)PTHREAD_ONCE_INIT;
    g_io_uring_ctx = NULL;
    
    /* Force reinitialization */
    init_io_uring_ctx();
}

/* ===================================================================
 * Directory Cache (avoids repeated ensure_directory calls)
 * ===================================================================*/

#define DIR_CACHE_SIZE 1024

typedef struct {
    char paths[DIR_CACHE_SIZE][PATH_MAX];
    int count;
    pthread_mutex_t lock;
} directory_cache_t;

static directory_cache_t g_dir_cache = {0};
static pthread_once_t g_dir_cache_once = PTHREAD_ONCE_INIT;

static void init_dir_cache(void)
{
    pthread_mutex_init(&g_dir_cache.lock, NULL);
    g_dir_cache.count = 0;
}

static bool is_dir_cached(const char *path)
{
    pthread_once(&g_dir_cache_once, init_dir_cache);
    
    pthread_mutex_lock(&g_dir_cache.lock);
    
    for (int i = 0; i < g_dir_cache.count; i++) {
        if (strcmp(g_dir_cache.paths[i], path) == 0) {
            pthread_mutex_unlock(&g_dir_cache.lock);
            return true;
        }
    }
    
    pthread_mutex_unlock(&g_dir_cache.lock);
    return false;
}

static void cache_dir(const char *path)
{
    pthread_once(&g_dir_cache_once, init_dir_cache);
    
    pthread_mutex_lock(&g_dir_cache.lock);
    
    /* Simple ring buffer - overwrite oldest if full */
    int idx = g_dir_cache.count % DIR_CACHE_SIZE;
    snprintf(g_dir_cache.paths[idx], PATH_MAX, "%s", path);
    
    if (g_dir_cache.count < DIR_CACHE_SIZE) {
        g_dir_cache.count++;
    }
    
    pthread_mutex_unlock(&g_dir_cache.lock);
}

static int ensure_directory_cached(const char *path)
{
    /* Fast path: already cached */
    if (is_dir_cached(path)) {
        return BUCKETS_OK;
    }
    
    /* Slow path: create and cache */
    int ret = buckets_ensure_directory(path);
    if (ret == BUCKETS_OK) {
        cache_dir(path);
    }
    
    return ret;
}

/* ===================================================================
 * Chunk I/O Operations
 * ===================================================================*/

/* Read chunk from disk */
int buckets_read_chunk(const char *disk_path, const char *object_path,
                       u32 chunk_index, void **data, size_t *size)
{
    if (!disk_path || !object_path || !data || !size) {
        buckets_error("NULL parameter in read_chunk");
        return -1;
    }

    /* Construct chunk path */
    char chunk_path[PATH_MAX];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%spart.%u",
             disk_path, object_path, chunk_index);

    /* Read chunk file */
    if (buckets_atomic_read(chunk_path, data, size) != 0) {
        buckets_error("Failed to read chunk: %s", chunk_path);
        return -1;
    }

    return 0;
}

/* Write chunk to disk with group commit optimization */
/* Async write completion context */
typedef struct {
    volatile int *result_ptr;        /* Pointer to store result (atomic access) */
    volatile int *done_flag;         /* Flag to indicate completion */
    int fd;                          /* File descriptor to close */
    char chunk_path[PATH_MAX];      /* Path for error cleanup */
} async_write_ctx_t;

/* Completion callback for async write */
static void chunk_write_completion_cb(buckets_io_result_t *result)
{
    async_write_ctx_t *ctx = (async_write_ctx_t*)result->user_data;
    
    if (result->result < 0) {
        buckets_error("Async write failed: %s", strerror(result->error));
        __atomic_store_n(ctx->result_ptr, -1, __ATOMIC_RELEASE);
        unlink(ctx->chunk_path);
    } else {
        __atomic_store_n(ctx->result_ptr, 0, __ATOMIC_RELEASE);
    }
    
    /* Close fd */
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    
    /* Signal completion */
    __atomic_store_n(ctx->done_flag, 1, __ATOMIC_RELEASE);
    
    buckets_free(ctx);
}

int buckets_write_chunk(const char *disk_path, const char *object_path,
                        u32 chunk_index, const void *data, size_t size)
{
    if (!disk_path || !object_path || !data) {
        buckets_error("NULL parameter in write_chunk");
        return -1;
    }

    /* Construct chunk path */
    char chunk_path[PATH_MAX];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%spart.%u",
             disk_path, object_path, chunk_index);

    /* Try io_uring first for async I/O */
    buckets_io_uring_context_t *io_ctx = buckets_chunk_get_io_uring_ctx();
    
    if (io_ctx) {
        /* Async path with io_uring */
        
        /* Ensure parent directory exists (with caching) */
        char *path_copy = buckets_strdup(chunk_path);
        char *dir_name = dirname(path_copy);
        char *dir_path = buckets_strdup(dir_name);
        buckets_free(path_copy);
        
        int ret = ensure_directory_cached(dir_path);
        buckets_free(dir_path);
        
        if (ret != BUCKETS_OK) {
            return -1;
        }
        
        /* Open file for writing (without O_DIRECT for now - requires aligned buffers) */
        int fd = open(chunk_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            buckets_error("Failed to open chunk file %s: %s", chunk_path, strerror(errno));
            return -1;
        }
        
        /* Prepare completion context */
        async_write_ctx_t *async_ctx = buckets_malloc(sizeof(*async_ctx));
        if (!async_ctx) {
            close(fd);
            return -1;
        }
        
        volatile int write_result = -1;
        volatile int done_flag = 0;
        
        async_ctx->result_ptr = &write_result;
        async_ctx->done_flag = &done_flag;
        async_ctx->fd = fd;
        snprintf(async_ctx->chunk_path, sizeof(async_ctx->chunk_path), "%s", chunk_path);
        
        /* Submit async write */
        ret = buckets_io_uring_write_async(io_ctx, fd, data, size,
                                           chunk_write_completion_cb, async_ctx);
        if (ret < 0) {
            buckets_error("Failed to submit async write");
            close(fd);
            buckets_free(async_ctx);
            goto fallback_blocking;
        }
        
        /* Submit to kernel - the poller thread will process completions */
        int submitted = buckets_io_uring_submit(io_ctx);
        if (submitted < 0) {
            buckets_error("Failed to submit io_uring operations");
            close(fd);
            buckets_free(async_ctx);
            goto fallback_blocking;
        }
        
        /* Wait for completion by polling the done flag
         * The background poller thread will call our callback which sets this flag.
         * Poll very frequently (10 microsecond intervals) for low latency. */
        int timeout_us = 30000000;  /* 30 seconds in microseconds */
        int elapsed_us = 0;
        int poll_interval_us = 10;  /* Check every 10 microseconds */
        
        while (!__atomic_load_n(&done_flag, __ATOMIC_ACQUIRE) && elapsed_us < timeout_us) {
            usleep(poll_interval_us);
            elapsed_us += poll_interval_us;
            
            /* Gradually increase poll interval to reduce CPU usage for slow operations */
            if (elapsed_us > 1000 && poll_interval_us < 100) {
                poll_interval_us = 100;  /* After 1ms, poll every 100us */
            } else if (elapsed_us > 10000 && poll_interval_us < 1000) {
                poll_interval_us = 1000;  /* After 10ms, poll every 1ms */
            }
        }
        
        if (!__atomic_load_n(&done_flag, __ATOMIC_ACQUIRE)) {
            buckets_error("Async write timeout after %dus (%dms)", timeout_us, timeout_us / 1000);
            return -1;
        }
        
        /* Get the result */
        int final_result = __atomic_load_n(&write_result, __ATOMIC_ACQUIRE);
        
        return final_result;
    }
    
fallback_blocking:
    /* Fallback: Try to use group commit if available */
    ;  /* Empty statement to allow label */
    buckets_group_commit_context_t *gc_ctx = buckets_storage_get_group_commit_ctx();
    
    if (gc_ctx) {
        /* Optimized path: write with group commit (batched fsync) */
        static bool logged_once = false;
        if (!logged_once) {
            buckets_info("✓ Group commit enabled for chunk writes (batched fsync)");
            logged_once = true;
        }
        
        /* Ensure parent directory exists (with caching) */
        char *path_copy = buckets_strdup(chunk_path);
        char *dir_name = dirname(path_copy);
        char *dir_path = buckets_strdup(dir_name);
        buckets_free(path_copy);
        
        int ret = ensure_directory_cached(dir_path);
        buckets_free(dir_path);
        
        if (ret != BUCKETS_OK) {
            return -1;
        }
        
        /* Open file for writing */
        int fd = open(chunk_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            buckets_error("Failed to open chunk file %s: %s", chunk_path, strerror(errno));
            return -1;
        }
        
        /* Write with group commit */
        ssize_t written = buckets_group_commit_write(gc_ctx, fd, data, size);
        
        if (written != (ssize_t)size) {
            close(fd);
            buckets_error("Failed to write chunk %s: written=%zd expected=%zu", 
                         chunk_path, written, size);
            unlink(chunk_path);
            return -1;
        }
        
        /* Flush this fd before closing (important!) */
        if (buckets_group_commit_flush_fd(gc_ctx, fd) != 0) {
            close(fd);
            buckets_error("Failed to flush chunk %s", chunk_path);
            unlink(chunk_path);
            return -1;
        }
        
        close(fd);
        return 0;
    } else {
        /* Fallback: use atomic write with immediate fsync */
        if (buckets_atomic_write(chunk_path, data, size) != 0) {
            buckets_error("Failed to write chunk: %s", chunk_path);
            return -1;
        }
        return 0;
    }
}

/* Verify chunk checksum */
bool buckets_verify_chunk(const void *data, size_t size,
                          const buckets_checksum_t *checksum)
{
    if (!data || !checksum) {
        buckets_error("NULL parameter in verify_chunk");
        return false;
    }

    /* Only BLAKE2b-256 supported for now */
    if (strcmp(checksum->algo, "BLAKE2b-256") != 0) {
        buckets_error("Unsupported checksum algorithm: %s", checksum->algo);
        return false;
    }

    /* Compute BLAKE2b-256 hash */
    u8 computed[32];
    buckets_blake2b(computed, 32, data, size, NULL, 0);

    /* Constant-time verification */
    return buckets_blake2b_verify(computed, checksum->hash, 32);
}

/* Compute chunk checksum */
int buckets_compute_chunk_checksum(const void *data, size_t size,
                                   buckets_checksum_t *checksum)
{
    if (!data || !checksum) {
        buckets_error("NULL parameter in compute_chunk_checksum");
        return -1;
    }

    /* Use BLAKE2b-256 */
    strcpy(checksum->algo, "BLAKE2b-256");
    buckets_blake2b(checksum->hash, 32, data, size, NULL, 0);

    return 0;
}

/* Delete chunk from disk */
int buckets_delete_chunk(const char *disk_path, const char *object_path,
                         u32 chunk_index)
{
    if (!disk_path || !object_path) {
        buckets_error("NULL parameter in delete_chunk");
        return -1;
    }

    /* Construct chunk path */
    char chunk_path[PATH_MAX];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%spart.%u",
             disk_path, object_path, chunk_index);

    /* Delete chunk file */
    if (unlink(chunk_path) != 0) {
        buckets_error("Failed to delete chunk: %s", chunk_path);
        return -1;
    }

    return 0;
}

/* Check if chunk exists */
bool buckets_chunk_exists(const char *disk_path, const char *object_path,
                          u32 chunk_index)
{
    if (!disk_path || !object_path) {
        return false;
    }

    /* Construct chunk path */
    char chunk_path[PATH_MAX];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%spart.%u",
             disk_path, object_path, chunk_index);

    /* Check if file exists */
    return (access(chunk_path, F_OK) == 0);
}
