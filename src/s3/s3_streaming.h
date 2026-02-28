/**
 * Streaming S3 Handler
 * 
 * Handles S3 PUT requests with streaming body processing.
 * Writes data chunks to disk as they arrive, without buffering
 * the entire body in memory.
 */

#ifndef BUCKETS_S3_STREAMING_H
#define BUCKETS_S3_STREAMING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "../net/uv_server_internal.h"
#include "../net/async_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================
 * Configuration
 * ===================================================================*/

/* Chunk size for streaming writes (256KB) */
#define S3_STREAM_CHUNK_SIZE    (256 * 1024)

/* Maximum pending async writes before applying backpressure */
#define S3_STREAM_MAX_PENDING   16

/* ===================================================================
 * Streaming Upload State
 * ===================================================================*/

/**
 * State for a streaming upload
 */
typedef struct {
    /* Request info */
    char bucket[256];
    char key[1024];
    char object_path[512];         /* Computed hash path */
    size_t content_length;         /* Expected size (0 if chunked) */
    size_t bytes_received;         /* Bytes received so far */
    
    /* Content type and metadata */
    char content_type[128];
    char etag[65];                 /* Computed MD5/BLAKE2b */
    
    /* Chunk buffering */
    uint8_t *chunk_buffer;         /* Current chunk being assembled */
    size_t chunk_buffer_len;       /* Bytes in current chunk */
    size_t chunk_buffer_capacity;  /* Chunk buffer size */
    
    /* Erasure coding state */
    uint32_t ec_k;                 /* Data chunks */
    uint32_t ec_m;                 /* Parity chunks */
    uint8_t **data_chunks;         /* Accumulated data chunks */
    uint32_t data_chunk_count;     /* Number of complete data chunks */
    
    /* Async I/O tracking */
    async_io_ctx_t *io_ctx;        /* Async I/O context */
    int pending_writes;            /* Number of pending async writes */
    int failed_writes;             /* Number of failed writes */
    bool write_error;              /* Write error occurred */
    
    /* Hash computation (incremental) */
    void *hash_ctx;                /* BLAKE2b context for streaming hash */
    
    /* Connection reference */
    uv_http_conn_t *conn;
    
    /* Completion tracking */
    bool headers_sent;
    bool completed;
    bool aborted;
} s3_stream_upload_t;

/**
 * State for a streaming download
 */
typedef struct {
    char bucket[256];
    char key[1024];
    char object_path[512];
    
    /* Object metadata */
    size_t object_size;
    char etag[65];
    char content_type[128];
    
    /* Range request */
    bool range_request;
    size_t range_start;
    size_t range_end;
    
    /* Streaming state */
    size_t bytes_sent;
    bool headers_sent;
    bool completed;
    
    /* Connection reference */
    uv_http_conn_t *conn;
    async_io_ctx_t *io_ctx;
} s3_stream_download_t;

/* ===================================================================
 * Streaming Handler API
 * ===================================================================*/

/**
 * Get streaming handler for S3 PUT operations
 * 
 * @return Streaming handler structure
 */
uv_stream_handler_t* s3_streaming_put_handler(void);

/**
 * Initialize streaming S3 subsystem
 * 
 * @param io_ctx Async I/O context for disk operations
 * @return 0 on success
 */
int s3_streaming_init(async_io_ctx_t *io_ctx);

/**
 * Cleanup streaming S3 subsystem
 */
void s3_streaming_cleanup(void);

/**
 * Create a new streaming upload state
 * 
 * @param conn HTTP connection
 * @param bucket Bucket name
 * @param key Object key
 * @param content_length Expected content length (0 if chunked)
 * @return Upload state or NULL on error
 */
s3_stream_upload_t* s3_stream_upload_create(uv_http_conn_t *conn,
                                             const char *bucket,
                                             const char *key,
                                             size_t content_length);

/**
 * Process a chunk of upload data
 * 
 * @param upload Upload state
 * @param data Chunk data
 * @param len Chunk length
 * @return 0 on success, -1 on error
 */
int s3_stream_upload_process(s3_stream_upload_t *upload,
                              const void *data, size_t len);

/**
 * Complete the upload (all data received)
 * 
 * @param upload Upload state
 * @return 0 on success, -1 on error
 */
int s3_stream_upload_complete(s3_stream_upload_t *upload);

/**
 * Abort the upload
 * 
 * @param upload Upload state
 */
void s3_stream_upload_abort(s3_stream_upload_t *upload);

/**
 * Free upload state
 * 
 * @param upload Upload state
 */
void s3_stream_upload_free(s3_stream_upload_t *upload);

/* ===================================================================
 * Internal Callbacks
 * ===================================================================*/

/* Streaming handler callbacks (registered with UV server) */
int s3_stream_on_request_start(uv_stream_request_t *req, void *user_data);
int s3_stream_on_body_chunk(uv_stream_request_t *req,
                             const void *data, size_t len,
                             void *user_data);
int s3_stream_on_request_complete(uv_stream_request_t *req, void *user_data);
void s3_stream_on_request_error(uv_stream_request_t *req, int error, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_S3_STREAMING_H */
