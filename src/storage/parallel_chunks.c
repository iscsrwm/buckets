/**
 * Parallel Chunk Operations
 * 
 * High-performance parallel chunk writes and reads using thread pools.
 * Improves distributed storage performance by executing RPC calls concurrently.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_placement.h"

/* Maximum number of concurrent chunk operations */
#define MAX_PARALLEL_CHUNKS 32

/**
 * Chunk operation task for thread pool
 */
typedef struct {
    /* Common fields */
    u32 chunk_index;           /* 1-based chunk index */
    char bucket[256];          /* Bucket name */
    char object[1024];         /* Object key */
    char disk_path[512];       /* Disk path (local or for RPC) */
    char node_endpoint[256];   /* Node endpoint for RPC */
    bool is_local;             /* True if local write, false if RPC */
    
    /* Write-specific fields */
    const void *chunk_data;    /* Chunk data pointer (write only) */
    size_t chunk_size;         /* Chunk size (write/read) */
    
    /* Read-specific fields */
    void *chunk_data_out;      /* Output buffer for read data */
    
    /* Result fields */
    int result;                /* Operation result (0=success, -1=error) */
    pthread_t thread;          /* Thread handle */
} chunk_task_t;

/* ===================================================================
 * Thread Worker Functions
 * ===================================================================*/

/**
 * Worker thread for chunk write
 */
static void* chunk_write_worker(void *arg)
{
    chunk_task_t *task = (chunk_task_t*)arg;
    
    if (task->is_local) {
        /* Local write */
        extern int buckets_write_chunk(const char *disk_path, const char *object_path,
                                       u32 chunk_index, const void *data, size_t size);
        
        /* Construct object path */
        char object_path[1536];
        snprintf(object_path, sizeof(object_path), "%s/%s", task->bucket, task->object);
        
        task->result = buckets_write_chunk(task->disk_path, object_path, 
                                          task->chunk_index, task->chunk_data, 
                                          task->chunk_size);
        
        if (task->result == 0) {
            buckets_debug("Parallel: Wrote chunk %u locally to %s", 
                         task->chunk_index, task->disk_path);
        } else {
            buckets_error("Parallel: Failed to write chunk %u locally to %s",
                         task->chunk_index, task->disk_path);
        }
    } else {
        /* Remote RPC write */
        extern int buckets_distributed_write_chunk(const char *peer_endpoint,
                                                   const char *bucket,
                                                   const char *object,
                                                   u32 chunk_index,
                                                   const void *chunk_data,
                                                   size_t chunk_size,
                                                   const char *disk_path);
        
        task->result = buckets_distributed_write_chunk(task->node_endpoint,
                                                      task->bucket,
                                                      task->object,
                                                      task->chunk_index,
                                                      task->chunk_data,
                                                      task->chunk_size,
                                                      task->disk_path);
        
        if (task->result == 0) {
            buckets_debug("Parallel: Wrote chunk %u via RPC to %s:%s",
                         task->chunk_index, task->node_endpoint, task->disk_path);
        } else {
            buckets_error("Parallel: Failed to write chunk %u via RPC to %s:%s",
                         task->chunk_index, task->node_endpoint, task->disk_path);
        }
    }
    
    return NULL;
}

/**
 * Worker thread for chunk read
 */
static void* chunk_read_worker(void *arg)
{
    chunk_task_t *task = (chunk_task_t*)arg;
    
    if (task->is_local) {
        /* Local read */
        extern int buckets_read_chunk(const char *disk_path, const char *object_path,
                                      u32 chunk_index, void **data, size_t *size);
        
        /* Construct object path */
        char object_path[1536];
        snprintf(object_path, sizeof(object_path), "%s/%s", task->bucket, task->object);
        
        void *chunk_data = NULL;
        size_t chunk_size = 0;
        
        task->result = buckets_read_chunk(task->disk_path, object_path,
                                         task->chunk_index, &chunk_data, &chunk_size);
        
        if (task->result == 0) {
            task->chunk_data_out = chunk_data;
            task->chunk_size = chunk_size;
            buckets_debug("Parallel: Read chunk %u locally from %s (size=%zu)",
                         task->chunk_index, task->disk_path, chunk_size);
        } else {
            buckets_error("Parallel: Failed to read chunk %u locally from %s",
                         task->chunk_index, task->disk_path);
        }
    } else {
        /* Remote RPC read */
        extern int buckets_distributed_read_chunk(const char *peer_endpoint,
                                                  const char *bucket,
                                                  const char *object,
                                                  u32 chunk_index,
                                                  void **chunk_data,
                                                  size_t *chunk_size,
                                                  const char *disk_path);
        
        void *chunk_data = NULL;
        size_t chunk_size = 0;
        
        task->result = buckets_distributed_read_chunk(task->node_endpoint,
                                                     task->bucket,
                                                     task->object,
                                                     task->chunk_index,
                                                     &chunk_data,
                                                     &chunk_size,
                                                     task->disk_path);
        
        if (task->result == 0) {
            task->chunk_data_out = chunk_data;
            task->chunk_size = chunk_size;
            buckets_debug("Parallel: Read chunk %u via RPC from %s:%s (size=%zu)",
                         task->chunk_index, task->node_endpoint, task->disk_path, chunk_size);
        } else {
            buckets_error("Parallel: Failed to read chunk %u via RPC from %s:%s",
                         task->chunk_index, task->node_endpoint, task->disk_path);
        }
    }
    
    return NULL;
}

/* ===================================================================
 * Public API
 * ===================================================================*/

/**
 * Write multiple chunks in parallel (local + RPC)
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param placement Placement result with disk endpoints
 * @param chunk_data_array Array of chunk data pointers (K+M chunks)
 * @param chunk_size Chunk size (same for all chunks)
 * @param num_chunks Total number of chunks (K+M)
 * @return 0 on success, -1 on error
 */
int buckets_parallel_write_chunks(const char *bucket,
                                   const char *object,
                                   buckets_placement_result_t *placement,
                                   const void **chunk_data_array,
                                   size_t chunk_size,
                                   u32 num_chunks)
{
    if (!bucket || !object || !placement || !chunk_data_array) {
        buckets_error("Invalid parameters for parallel write");
        return -1;
    }
    
    if (num_chunks > MAX_PARALLEL_CHUNKS) {
        buckets_error("Too many chunks: %u (max %d)", num_chunks, MAX_PARALLEL_CHUNKS);
        return -1;
    }
    
    buckets_info("Parallel write: %u chunks for %s/%s", num_chunks, bucket, object);
    
    /* Check if we have endpoints for distributed mode */
    bool has_endpoints = (placement->disk_endpoints && 
                         placement->disk_endpoints[0] && 
                         placement->disk_endpoints[0][0] != '\0');
    
    /* Allocate task array */
    chunk_task_t *tasks = buckets_calloc(num_chunks, sizeof(chunk_task_t));
    if (!tasks) {
        return -1;
    }
    
    /* Initialize tasks */
    for (u32 i = 0; i < num_chunks; i++) {
        chunk_task_t *task = &tasks[i];
        
        task->chunk_index = i + 1;
        strncpy(task->bucket, bucket, sizeof(task->bucket) - 1);
        strncpy(task->object, object, sizeof(task->object) - 1);
        strncpy(task->disk_path, placement->disk_paths[i], sizeof(task->disk_path) - 1);
        
        task->chunk_data = chunk_data_array[i];
        task->chunk_size = chunk_size;
        
        /* Determine if local or remote */
        if (has_endpoints) {
            extern bool buckets_distributed_is_local_disk(const char *disk_endpoint);
            task->is_local = buckets_distributed_is_local_disk(placement->disk_endpoints[i]);
            
            if (!task->is_local) {
                extern int buckets_distributed_extract_node_endpoint(const char *disk_endpoint,
                                                                     char *node_endpoint, size_t size);
                if (buckets_distributed_extract_node_endpoint(placement->disk_endpoints[i],
                                                             task->node_endpoint,
                                                             sizeof(task->node_endpoint)) != 0) {
                    buckets_error("Failed to extract node endpoint from: %s",
                                 placement->disk_endpoints[i]);
                    task->is_local = true;  /* Fall back to local */
                }
            }
        } else {
            task->is_local = true;  /* No endpoints = local only mode */
        }
        
        task->result = -1;  /* Initialize as failed */
    }
    
    /* Launch threads */
    for (u32 i = 0; i < num_chunks; i++) {
        int ret = pthread_create(&tasks[i].thread, NULL, chunk_write_worker, &tasks[i]);
        if (ret != 0) {
            buckets_error("Failed to create thread for chunk %u: %d", i + 1, ret);
            /* Mark as failed but continue launching other threads */
            tasks[i].result = -1;
        }
    }
    
    /* Wait for all threads to complete */
    int failed_count = 0;
    for (u32 i = 0; i < num_chunks; i++) {
        if (tasks[i].thread != 0) {
            pthread_join(tasks[i].thread, NULL);
        }
        
        if (tasks[i].result != 0) {
            buckets_error("Chunk %u write failed", i + 1);
            failed_count++;
        }
    }
    
    buckets_free(tasks);
    
    if (failed_count > 0) {
        buckets_error("Parallel write: %d/%u chunks failed", failed_count, num_chunks);
        return -1;
    }
    
    buckets_info("Parallel write: All %u chunks written successfully", num_chunks);
    return 0;
}

/**
 * Read multiple chunks in parallel (local + RPC)
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param placement Placement result with disk endpoints
 * @param chunk_data_array Output: Array of chunk data pointers (caller must free each)
 * @param chunk_sizes_array Output: Array of chunk sizes
 * @param num_chunks Total number of chunks to read (K+M)
 * @return Number of successfully read chunks (need >= K for reconstruction)
 */
int buckets_parallel_read_chunks(const char *bucket,
                                  const char *object,
                                  buckets_placement_result_t *placement,
                                  void **chunk_data_array,
                                  size_t *chunk_sizes_array,
                                  u32 num_chunks)
{
    if (!bucket || !object || !placement || !chunk_data_array || !chunk_sizes_array) {
        buckets_error("Invalid parameters for parallel read");
        return -1;
    }
    
    if (num_chunks > MAX_PARALLEL_CHUNKS) {
        buckets_error("Too many chunks: %u (max %d)", num_chunks, MAX_PARALLEL_CHUNKS);
        return -1;
    }
    
    buckets_info("Parallel read: %u chunks for %s/%s", num_chunks, bucket, object);
    
    /* Check if we have endpoints for distributed mode */
    bool has_endpoints = (placement->disk_endpoints && 
                         placement->disk_endpoints[0] && 
                         placement->disk_endpoints[0][0] != '\0');
    
    /* Allocate task array */
    chunk_task_t *tasks = buckets_calloc(num_chunks, sizeof(chunk_task_t));
    if (!tasks) {
        return -1;
    }
    
    /* Initialize tasks */
    for (u32 i = 0; i < num_chunks; i++) {
        chunk_task_t *task = &tasks[i];
        
        task->chunk_index = i + 1;
        strncpy(task->bucket, bucket, sizeof(task->bucket) - 1);
        strncpy(task->object, object, sizeof(task->object) - 1);
        strncpy(task->disk_path, placement->disk_paths[i], sizeof(task->disk_path) - 1);
        
        task->chunk_data_out = NULL;
        task->chunk_size = 0;
        
        /* Determine if local or remote */
        if (has_endpoints) {
            extern bool buckets_distributed_is_local_disk(const char *disk_endpoint);
            task->is_local = buckets_distributed_is_local_disk(placement->disk_endpoints[i]);
            
            if (!task->is_local) {
                extern int buckets_distributed_extract_node_endpoint(const char *disk_endpoint,
                                                                     char *node_endpoint, size_t size);
                if (buckets_distributed_extract_node_endpoint(placement->disk_endpoints[i],
                                                             task->node_endpoint,
                                                             sizeof(task->node_endpoint)) != 0) {
                    buckets_error("Failed to extract node endpoint from: %s",
                                 placement->disk_endpoints[i]);
                    task->is_local = true;  /* Fall back to local */
                }
            }
        } else {
            task->is_local = true;  /* No endpoints = local only mode */
        }
        
        task->result = -1;  /* Initialize as failed */
    }
    
    /* Launch threads */
    for (u32 i = 0; i < num_chunks; i++) {
        int ret = pthread_create(&tasks[i].thread, NULL, chunk_read_worker, &tasks[i]);
        if (ret != 0) {
            buckets_error("Failed to create thread for chunk %u: %d", i + 1, ret);
            tasks[i].result = -1;
        }
    }
    
    /* Wait for all threads to complete */
    int success_count = 0;
    for (u32 i = 0; i < num_chunks; i++) {
        if (tasks[i].thread != 0) {
            pthread_join(tasks[i].thread, NULL);
        }
        
        if (tasks[i].result == 0) {
            /* Success - copy output */
            chunk_data_array[i] = tasks[i].chunk_data_out;
            chunk_sizes_array[i] = tasks[i].chunk_size;
            success_count++;
        } else {
            /* Failed - set to NULL */
            chunk_data_array[i] = NULL;
            chunk_sizes_array[i] = 0;
        }
    }
    
    buckets_free(tasks);
    
    buckets_info("Parallel read: %d/%u chunks read successfully", success_count, num_chunks);
    return success_count;
}

/* ===================================================================
 * Parallel Metadata Write Operations
 * ===================================================================*/

/**
 * Metadata write task for thread pool
 */
typedef struct {
    u32 disk_index;                /* Disk index (1-based) */
    char bucket[256];              /* Bucket name */
    char object[1024];             /* Object key */
    char object_path[1536];        /* Full object path */
    char disk_path[512];           /* Disk path */
    char node_endpoint[256];       /* Node endpoint for RPC */
    bool is_local;                 /* True if local write */
    
    buckets_xl_meta_t meta;        /* Metadata to write (copied) */
    
    int result;                    /* Operation result */
    pthread_t thread;              /* Thread handle */
} metadata_task_t;

/**
 * Worker thread for metadata write
 */
static void* metadata_write_worker(void *arg)
{
    metadata_task_t *task = (metadata_task_t*)arg;
    
    if (task->is_local) {
        /* Local write */
        extern int buckets_write_xl_meta(const char *disk_path, const char *object_path,
                                         const buckets_xl_meta_t *meta);
        
        task->result = buckets_write_xl_meta(task->disk_path, task->object_path, &task->meta);
        
        if (task->result == 0) {
            buckets_debug("Parallel metadata: Wrote to local disk %s", task->disk_path);
        }
    } else {
        /* Remote RPC write */
        extern int buckets_distributed_write_xlmeta(const char *peer_endpoint,
                                                    const char *bucket, const char *object,
                                                    const char *disk_path,
                                                    const buckets_xl_meta_t *meta);
        
        task->result = buckets_distributed_write_xlmeta(task->node_endpoint,
                                                       task->bucket,
                                                       task->object,
                                                       task->disk_path,
                                                       &task->meta);
        
        if (task->result == 0) {
            buckets_debug("Parallel metadata: Wrote via RPC to %s:%s",
                         task->node_endpoint, task->disk_path);
        }
    }
    
    return NULL;
}

/**
 * Write xl.meta to multiple disks in parallel
 */
int buckets_parallel_write_metadata(const char *bucket,
                                     const char *object,
                                     const char *object_path,
                                     buckets_placement_result_t *placement,
                                     char **disk_paths,
                                     const buckets_xl_meta_t *base_meta,
                                     u32 num_disks,
                                     bool has_endpoints)
{
    if (!bucket || !object || !object_path || !disk_paths || !base_meta) {
        buckets_error("Invalid parameters for parallel metadata write");
        return -1;
    }
    
    if (num_disks > MAX_PARALLEL_CHUNKS) {
        buckets_error("Too many disks: %u (max %d)", num_disks, MAX_PARALLEL_CHUNKS);
        return -1;
    }
    
    /* Allocate task array */
    metadata_task_t *tasks = buckets_calloc(num_disks, sizeof(metadata_task_t));
    if (!tasks) {
        return -1;
    }
    
    /* Initialize tasks */
    for (u32 i = 0; i < num_disks; i++) {
        metadata_task_t *task = &tasks[i];
        
        task->disk_index = i + 1;
        strncpy(task->bucket, bucket, sizeof(task->bucket) - 1);
        strncpy(task->object, object, sizeof(task->object) - 1);
        strncpy(task->object_path, object_path, sizeof(task->object_path) - 1);
        strncpy(task->disk_path, disk_paths[i], sizeof(task->disk_path) - 1);
        
        /* Deep copy metadata and update disk index */
        memcpy(&task->meta, base_meta, sizeof(buckets_xl_meta_t));
        task->meta.erasure.index = i + 1;
        
        /* Determine if local or remote */
        if (has_endpoints && placement && placement->disk_endpoints) {
            extern bool buckets_distributed_is_local_disk(const char *disk_endpoint);
            task->is_local = buckets_distributed_is_local_disk(placement->disk_endpoints[i]);
            
            if (!task->is_local) {
                extern int buckets_distributed_extract_node_endpoint(const char *disk_endpoint,
                                                                    char *node_endpoint, size_t size);
                if (buckets_distributed_extract_node_endpoint(placement->disk_endpoints[i],
                                                             task->node_endpoint,
                                                             sizeof(task->node_endpoint)) != 0) {
                    buckets_error("Failed to extract node endpoint from: %s",
                                 placement->disk_endpoints[i]);
                    task->is_local = true;  /* Fall back to local */
                }
            }
        } else {
            task->is_local = true;
        }
        
        task->result = -1;  /* Initialize as failed */
    }
    
    /* Launch threads */
    for (u32 i = 0; i < num_disks; i++) {
        int ret = pthread_create(&tasks[i].thread, NULL, metadata_write_worker, &tasks[i]);
        if (ret != 0) {
            buckets_error("Failed to create thread for metadata write to disk %u: %d", 
                         i + 1, ret);
            tasks[i].result = -1;
        }
    }
    
    /* Wait for all threads to complete */
    int failed_count = 0;
    for (u32 i = 0; i < num_disks; i++) {
        if (tasks[i].thread != 0) {
            pthread_join(tasks[i].thread, NULL);
        }
        
        if (tasks[i].result != 0) {
            buckets_error("Metadata write to disk %s failed", disk_paths[i]);
            failed_count++;
        }
    }
    
    buckets_free(tasks);
    
    if (failed_count > 0) {
        buckets_error("Parallel metadata write: %d/%u disks failed", failed_count, num_disks);
        return -1;
    }
    
    buckets_info("Parallel metadata write: All %u disks succeeded", num_disks);
    return 0;
}
