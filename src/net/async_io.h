/**
 * Async I/O Module
 * 
 * Provides asynchronous disk I/O operations using libuv's thread pool.
 * Wraps synchronous storage operations for non-blocking use in event loop.
 */

#ifndef BUCKETS_ASYNC_IO_H
#define BUCKETS_ASYNC_IO_H

#include <uv.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * Types
 * ===================================================================*/

/* Forward declarations */
typedef struct async_io_ctx async_io_ctx_t;
typedef struct async_write_req async_write_req_t;
typedef struct async_read_req async_read_req_t;

/* Completion callbacks */
typedef void (*async_write_cb)(async_write_req_t *req, int status);
typedef void (*async_read_cb)(async_read_req_t *req, int status, 
                               void *data, size_t size);

/**
 * Async I/O context
 * 
 * Manages the libuv loop reference and configuration.
 */
struct async_io_ctx {
    uv_loop_t *loop;           /* Event loop (not owned) */
    bool initialized;
};

/**
 * Async write request
 * 
 * Tracks an asynchronous file write operation.
 */
struct async_write_req {
    uv_work_t work;            /* libuv work request */
    async_io_ctx_t *ctx;       /* Context reference */
    
    /* Write parameters */
    char *path;                /* File path (owned, freed after completion) */
    void *data;                /* Data to write (owned if owns_data=true) */
    size_t size;               /* Data size */
    bool owns_data;            /* Should free data after write? */
    bool atomic;               /* Use atomic write (temp file + rename)? */
    
    /* Completion */
    async_write_cb callback;   /* User callback */
    void *user_data;           /* User data for callback */
    int status;                /* Result status (0=success) */
};

/**
 * Async read request
 * 
 * Tracks an asynchronous file read operation.
 */
struct async_read_req {
    uv_work_t work;            /* libuv work request */
    async_io_ctx_t *ctx;       /* Context reference */
    
    /* Read parameters */
    char *path;                /* File path (owned, freed after completion) */
    size_t max_size;           /* Maximum bytes to read (0=entire file) */
    
    /* Output */
    void *data;                /* Read data (allocated by worker) */
    size_t size;               /* Actual bytes read */
    
    /* Completion */
    async_read_cb callback;    /* User callback */
    void *user_data;           /* User data for callback */
    int status;                /* Result status (0=success) */
};

/**
 * Async mkdir request
 */
typedef struct {
    uv_work_t work;
    async_io_ctx_t *ctx;
    char *path;
    int mode;
    bool recursive;
    void (*callback)(void *user_data, int status);
    void *user_data;
    int status;
} async_mkdir_req_t;

/**
 * Async chunk write request
 * 
 * For writing erasure-coded chunks with metadata.
 */
typedef struct {
    uv_work_t work;
    async_io_ctx_t *ctx;
    
    /* Chunk info */
    char *disk_path;           /* Disk root path */
    char *object_path;         /* Object path within disk */
    uint32_t chunk_index;      /* Chunk index (1-based) */
    void *data;                /* Chunk data */
    size_t size;               /* Chunk size */
    bool owns_data;
    
    /* Completion */
    void (*callback)(void *user_data, int status);
    void *user_data;
    int status;
} async_chunk_write_req_t;

/**
 * Async chunk read request
 */
typedef struct {
    uv_work_t work;
    async_io_ctx_t *ctx;
    
    /* Chunk info */
    char *disk_path;
    char *object_path;
    uint32_t chunk_index;
    
    /* Output */
    void *data;
    size_t size;
    
    /* Completion */
    void (*callback)(void *user_data, int status, void *data, size_t size);
    void *user_data;
    int status;
} async_chunk_read_req_t;

/* ===================================================================
 * Initialization
 * ===================================================================*/

/**
 * Initialize async I/O context
 * 
 * @param ctx Context to initialize
 * @param loop libuv event loop (not owned, must outlive context)
 * @return 0 on success, error code otherwise
 */
int async_io_init(async_io_ctx_t *ctx, uv_loop_t *loop);

/**
 * Cleanup async I/O context
 * 
 * @param ctx Context to cleanup
 */
void async_io_cleanup(async_io_ctx_t *ctx);

/* ===================================================================
 * File Operations
 * ===================================================================*/

/**
 * Asynchronously write data to file
 * 
 * @param ctx Async I/O context
 * @param path File path
 * @param data Data to write
 * @param size Data size
 * @param owns_data If true, data will be freed after write
 * @param atomic If true, use atomic write (temp + rename)
 * @param callback Completion callback
 * @param user_data User data for callback
 * @return 0 on success (request queued), error code otherwise
 */
int async_io_write_file(async_io_ctx_t *ctx,
                        const char *path,
                        const void *data,
                        size_t size,
                        bool owns_data,
                        bool atomic,
                        async_write_cb callback,
                        void *user_data);

/**
 * Asynchronously read file
 * 
 * @param ctx Async I/O context
 * @param path File path
 * @param max_size Maximum bytes to read (0 for entire file)
 * @param callback Completion callback (receives allocated data)
 * @param user_data User data for callback
 * @return 0 on success (request queued), error code otherwise
 */
int async_io_read_file(async_io_ctx_t *ctx,
                       const char *path,
                       size_t max_size,
                       async_read_cb callback,
                       void *user_data);

/**
 * Asynchronously create directory
 * 
 * @param ctx Async I/O context
 * @param path Directory path
 * @param mode Directory permissions (e.g., 0755)
 * @param recursive Create parent directories if needed
 * @param callback Completion callback
 * @param user_data User data for callback
 * @return 0 on success (request queued), error code otherwise
 */
int async_io_mkdir(async_io_ctx_t *ctx,
                   const char *path,
                   int mode,
                   bool recursive,
                   void (*callback)(void *user_data, int status),
                   void *user_data);

/* ===================================================================
 * Chunk Operations
 * ===================================================================*/

/**
 * Asynchronously write chunk to disk
 * 
 * Creates the chunk file with proper directory structure:
 * {disk_path}/{object_path}/part.{chunk_index}
 * 
 * @param ctx Async I/O context
 * @param disk_path Disk root path
 * @param object_path Object path within disk
 * @param chunk_index Chunk index (1-based)
 * @param data Chunk data
 * @param size Chunk size
 * @param owns_data If true, data will be freed after write
 * @param callback Completion callback
 * @param user_data User data for callback
 * @return 0 on success (request queued), error code otherwise
 */
int async_io_write_chunk(async_io_ctx_t *ctx,
                         const char *disk_path,
                         const char *object_path,
                         uint32_t chunk_index,
                         const void *data,
                         size_t size,
                         bool owns_data,
                         void (*callback)(void *user_data, int status),
                         void *user_data);

/**
 * Asynchronously read chunk from disk
 * 
 * @param ctx Async I/O context
 * @param disk_path Disk root path
 * @param object_path Object path within disk
 * @param chunk_index Chunk index (1-based)
 * @param callback Completion callback (receives allocated data)
 * @param user_data User data for callback
 * @return 0 on success (request queued), error code otherwise
 */
int async_io_read_chunk(async_io_ctx_t *ctx,
                        const char *disk_path,
                        const char *object_path,
                        uint32_t chunk_index,
                        void (*callback)(void *user_data, int status, 
                                        void *data, size_t size),
                        void *user_data);

/* ===================================================================
 * Batch Operations
 * ===================================================================*/

/**
 * Batch write completion callback
 */
typedef void (*async_batch_write_cb)(void *user_data, int num_success, int num_failed);

/**
 * Batch write request
 */
typedef struct {
    async_io_ctx_t *ctx;
    
    /* Items to write */
    int total_count;
    int completed_count;
    int success_count;
    int failed_count;
    
    /* Completion */
    async_batch_write_cb callback;
    void *user_data;
    
    /* Synchronization */
    uv_mutex_t lock;
} async_batch_write_t;

/**
 * Start a batch write operation
 * 
 * @param ctx Async I/O context
 * @param count Number of writes in batch
 * @param callback Completion callback (called when all writes done)
 * @param user_data User data for callback
 * @return Batch handle or NULL on error
 */
async_batch_write_t* async_io_batch_write_start(async_io_ctx_t *ctx,
                                                 int count,
                                                 async_batch_write_cb callback,
                                                 void *user_data);

/**
 * Add a file write to batch
 * 
 * @param batch Batch handle
 * @param path File path
 * @param data Data to write
 * @param size Data size
 * @param owns_data If true, data will be freed after write
 * @param atomic Use atomic write
 * @return 0 on success, error code otherwise
 */
int async_io_batch_write_add(async_batch_write_t *batch,
                             const char *path,
                             const void *data,
                             size_t size,
                             bool owns_data,
                             bool atomic);

/**
 * Add a chunk write to batch
 * 
 * @param batch Batch handle
 * @param disk_path Disk root path
 * @param object_path Object path
 * @param chunk_index Chunk index
 * @param data Chunk data
 * @param size Chunk size
 * @param owns_data If true, data will be freed after write
 * @return 0 on success, error code otherwise
 */
int async_io_batch_write_add_chunk(async_batch_write_t *batch,
                                    const char *disk_path,
                                    const char *object_path,
                                    uint32_t chunk_index,
                                    const void *data,
                                    size_t size,
                                    bool owns_data);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_ASYNC_IO_H */
