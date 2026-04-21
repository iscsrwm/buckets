/**
 * Chunk I/O Implementation
 * 
 * Chunk read/write operations with checksum verification.
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

    /* Try to use group commit if available */
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
