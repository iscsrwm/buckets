/**
 * Streaming S3 Handler Implementation
 * 
 * Handles S3 PUT requests with streaming body processing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "buckets.h"
#include "buckets_net.h"
#include "buckets_storage.h"
#include "buckets_crypto.h"
#include "s3_streaming.h"
#include "../net/uv_server_internal.h"  /* For uv_http_server_add_async_route */

/* ===================================================================
 * Global State
 * ===================================================================*/

static async_io_ctx_t *g_io_ctx = NULL;
static uv_stream_handler_t g_put_handler;
static bool g_initialized = false;

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/* Parse bucket and key from URL */
static int parse_s3_url(const char *url, char *bucket, size_t bucket_size,
                        char *key, size_t key_size)
{
    if (!url || url[0] != '/') {
        return -1;
    }
    
    const char *p = url + 1;  /* Skip leading slash */
    
    /* Find bucket (up to first slash or query string) */
    const char *slash = strchr(p, '/');
    const char *question = strchr(p, '?');
    
    size_t bucket_len;
    if (slash) {
        bucket_len = (size_t)(slash - p);
    } else if (question) {
        bucket_len = (size_t)(question - p);
    } else {
        bucket_len = strlen(p);
    }
    
    if (bucket_len == 0 || bucket_len >= bucket_size) {
        return -1;
    }
    
    memcpy(bucket, p, bucket_len);
    bucket[bucket_len] = '\0';
    
    /* Extract key if present */
    if (slash) {
        const char *key_start = slash + 1;
        size_t key_len;
        
        if (question && question > key_start) {
            key_len = (size_t)(question - key_start);
        } else {
            key_len = strlen(key_start);
        }
        
        if (key_len >= key_size) {
            return -1;
        }
        
        memcpy(key, key_start, key_len);
        key[key_len] = '\0';
    } else {
        key[0] = '\0';
    }
    
    return 0;
}

/* URL decode in place */
static void url_decode_inplace(char *str)
{
    char *src = str;
    char *dst = str;
    
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Get header value (case-insensitive) */
static const char* get_header(uv_http_conn_t *conn, const char *name)
{
    return uv_http_get_header(conn, name);
}

/* Callback for user metadata header iteration */
static void stream_user_meta_callback(const char *name, const char *value, void *user_data)
{
    s3_stream_upload_t *upload = (s3_stream_upload_t *)user_data;
    
    if (upload->user_meta_count >= 32) {
        buckets_warn("Too many user metadata headers, ignoring: %s", name);
        return;
    }
    
    /* Strip "x-amz-meta-" prefix (11 characters) */
    const char *key = name + 11;
    
    upload->user_meta_keys[upload->user_meta_count] = buckets_strdup(key);
    upload->user_meta_values[upload->user_meta_count] = buckets_strdup(value);
    upload->user_meta_count++;
    
    buckets_debug("Streaming PUT: parsed user metadata: %s = %s", key, value);
}

/* Send error response */
static void send_error_response(uv_http_conn_t *conn, int status, const char *message)
{
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Error>\n"
        "  <Code>%d</Code>\n"
        "  <Message>%s</Message>\n"
        "</Error>",
        status, message);
    
    const char *headers[] = {
        "Content-Type", "application/xml",
        NULL
    };
    
    uv_http_response_start(conn, status, headers, 2, body_len);
    uv_http_response_write(conn, body, body_len);
}

/* Send success response for PUT */
static void send_put_success(uv_http_conn_t *conn, const char *etag)
{
    const char *headers[] = {
        "ETag", etag,
        NULL
    };
    
    uv_http_response_start(conn, 200, headers, 2, 0);
    uv_http_response_end(conn);  /* Send terminating chunk for chunked encoding */
}

/* ===================================================================
 * Upload State Management
 * ===================================================================*/

s3_stream_upload_t* s3_stream_upload_create(uv_http_conn_t *conn,
                                             const char *bucket,
                                             const char *key,
                                             size_t content_length)
{
    s3_stream_upload_t *upload = buckets_calloc(1, sizeof(s3_stream_upload_t));
    if (!upload) {
        return NULL;
    }
    
    upload->conn = conn;
    upload->io_ctx = g_io_ctx;
    upload->content_length = content_length;
    
    /* Copy bucket and key */
    strncpy(upload->bucket, bucket, sizeof(upload->bucket) - 1);
    strncpy(upload->key, key, sizeof(upload->key) - 1);
    
    /* Compute object path */
    buckets_compute_object_path(bucket, key, upload->object_path, sizeof(upload->object_path));
    
    /* Allocate chunk buffer */
    upload->chunk_buffer_capacity = S3_STREAM_CHUNK_SIZE;
    upload->chunk_buffer = buckets_malloc(upload->chunk_buffer_capacity);
    if (!upload->chunk_buffer) {
        buckets_free(upload);
        return NULL;
    }
    
    /* Initialize hash context for ETag computation */
    upload->hash_ctx = buckets_malloc(sizeof(buckets_blake2b_ctx_t));
    if (!upload->hash_ctx) {
        buckets_free(upload->chunk_buffer);
        buckets_free(upload);
        return NULL;
    }
    buckets_blake2b_init((buckets_blake2b_ctx_t*)upload->hash_ctx, 32);
    
    /* Default EC config - will be updated based on cluster */
    upload->ec_k = 2;
    upload->ec_m = 2;
    
    buckets_debug("Created streaming upload: bucket=%s, key=%s, content_length=%zu",
                 bucket, key, content_length);
    
    return upload;
}

void s3_stream_upload_free(s3_stream_upload_t *upload)
{
    if (!upload) return;
    
    if (upload->chunk_buffer) {
        buckets_free(upload->chunk_buffer);
    }
    
    if (upload->hash_ctx) {
        buckets_free(upload->hash_ctx);
    }
    
    /* Free accumulated data chunks */
    if (upload->data_chunks) {
        for (uint32_t i = 0; i < upload->data_chunk_count; i++) {
            if (upload->data_chunks[i]) {
                buckets_free(upload->data_chunks[i]);
            }
        }
        buckets_free(upload->data_chunks);
    }
    
    /* Free user metadata */
    for (int i = 0; i < upload->user_meta_count; i++) {
        buckets_free(upload->user_meta_keys[i]);
        buckets_free(upload->user_meta_values[i]);
    }
    
    buckets_free(upload);
}

/* Callback for async chunk write completion (will be used for async disk I/O) */
__attribute__((unused))
static void chunk_write_complete(void *user_data, int status)
{
    s3_stream_upload_t *upload = (s3_stream_upload_t*)user_data;
    
    upload->pending_writes--;
    
    if (status != 0) {
        upload->failed_writes++;
        upload->write_error = true;
        buckets_error("Chunk write failed for %s/%s", upload->bucket, upload->key);
    }
    
    buckets_debug("Chunk write complete: pending=%d, failed=%d",
                 upload->pending_writes, upload->failed_writes);
}

/* Flush current chunk buffer to disk */
static int flush_chunk_buffer(s3_stream_upload_t *upload)
{
    if (upload->chunk_buffer_len == 0) {
        return 0;
    }
    
    /* For now, just accumulate chunks in memory for small objects.
     * For large objects, we'd write to disk here using async I/O. */
    
    /* Grow data_chunks array */
    uint32_t new_count = upload->data_chunk_count + 1;
    uint8_t **new_chunks = buckets_realloc(upload->data_chunks, 
                                            new_count * sizeof(uint8_t*));
    if (!new_chunks) {
        return -1;
    }
    upload->data_chunks = new_chunks;
    
    /* Copy chunk data */
    upload->data_chunks[upload->data_chunk_count] = buckets_malloc(upload->chunk_buffer_len);
    if (!upload->data_chunks[upload->data_chunk_count]) {
        return -1;
    }
    
    memcpy(upload->data_chunks[upload->data_chunk_count], 
           upload->chunk_buffer, upload->chunk_buffer_len);
    upload->data_chunk_count++;
    
    /* Reset buffer */
    upload->chunk_buffer_len = 0;
    
    return 0;
}

int s3_stream_upload_process(s3_stream_upload_t *upload,
                              const void *data, size_t len)
{
    if (!upload || !data || upload->aborted) {
        return -1;
    }
    
    const uint8_t *src = (const uint8_t*)data;
    size_t remaining = len;
    
    /* Update hash */
    buckets_blake2b_update((buckets_blake2b_ctx_t*)upload->hash_ctx, data, len);
    
    upload->bytes_received += len;
    
    while (remaining > 0) {
        /* Fill chunk buffer */
        size_t space = upload->chunk_buffer_capacity - upload->chunk_buffer_len;
        size_t to_copy = (remaining < space) ? remaining : space;
        
        memcpy(upload->chunk_buffer + upload->chunk_buffer_len, src, to_copy);
        upload->chunk_buffer_len += to_copy;
        src += to_copy;
        remaining -= to_copy;
        
        /* Flush if buffer is full */
        if (upload->chunk_buffer_len >= upload->chunk_buffer_capacity) {
            if (flush_chunk_buffer(upload) != 0) {
                return -1;
            }
        }
    }
    
    return 0;
}

int s3_stream_upload_complete(s3_stream_upload_t *upload)
{
    if (!upload || upload->aborted) {
        return -1;
    }
    
    /* Flush any remaining data */
    if (upload->chunk_buffer_len > 0) {
        if (flush_chunk_buffer(upload) != 0) {
            return -1;
        }
    }
    
    /* Finalize hash */
    uint8_t hash[32];
    buckets_blake2b_final((buckets_blake2b_ctx_t*)upload->hash_ctx, hash, sizeof(hash));
    
    /* Format ETag */
    snprintf(upload->etag, sizeof(upload->etag), "\"%02x%02x%02x%02x%02x%02x%02x%02x"
             "%02x%02x%02x%02x%02x%02x%02x%02x\"",
             hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7],
             hash[8], hash[9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]);
    
    /* Reassemble complete object data */
    size_t total_size = upload->bytes_received;
    uint8_t *object_data = buckets_malloc(total_size);
    if (!object_data) {
        buckets_error("Failed to allocate %zu bytes for object data", total_size);
        return -1;
    }
    
    size_t offset = 0;
    for (uint32_t i = 0; i < upload->data_chunk_count; i++) {
        size_t chunk_size = (i < upload->data_chunk_count - 1) 
                            ? S3_STREAM_CHUNK_SIZE 
                            : (total_size - offset);
        memcpy(object_data + offset, upload->data_chunks[i], chunk_size);
        offset += chunk_size;
    }
    
    /* Write to storage using existing storage layer */
    int ret;
    
    /* Check if we have user metadata */
    if (upload->user_meta_count > 0) {
        /* Use put_object_with_metadata for user metadata support */
        buckets_xl_meta_t meta;
        memset(&meta, 0, sizeof(meta));
        
        /* Set content type */
        if (upload->content_type[0]) {
            meta.meta.content_type = buckets_strdup(upload->content_type);
        }
        
        /* Copy user metadata */
        for (int i = 0; i < upload->user_meta_count; i++) {
            buckets_add_user_metadata(&meta, upload->user_meta_keys[i], 
                                      upload->user_meta_values[i]);
            buckets_info("Streaming PUT: Adding user metadata: %s = %s", 
                        upload->user_meta_keys[i], upload->user_meta_values[i]);
        }
        
        ret = buckets_put_object_with_metadata(upload->bucket, upload->key, 
                                                object_data, total_size, 
                                                &meta, false, NULL);
        
        /* Free metadata strings */
        if (meta.meta.content_type) {
            buckets_free(meta.meta.content_type);
        }
        for (u32 i = 0; i < meta.meta.user_count; i++) {
            buckets_free(meta.meta.user_keys[i]);
            buckets_free(meta.meta.user_values[i]);
        }
        if (meta.meta.user_keys) buckets_free(meta.meta.user_keys);
        if (meta.meta.user_values) buckets_free(meta.meta.user_values);
    } else {
        /* No user metadata - use simple put */
        ret = buckets_put_object(upload->bucket, upload->key,
                                  object_data, total_size,
                                  upload->content_type[0] ? upload->content_type : NULL);
    }
    
    buckets_free(object_data);
    
    if (ret != 0) {
        buckets_error("Failed to store object %s/%s", upload->bucket, upload->key);
        return -1;
    }
    
    upload->completed = true;
    
    buckets_info("Streaming upload complete: %s/%s (%zu bytes, ETag=%s, user_meta=%d)",
                upload->bucket, upload->key, total_size, upload->etag, upload->user_meta_count);
    
    return 0;
}

void s3_stream_upload_abort(s3_stream_upload_t *upload)
{
    if (!upload) return;
    
    upload->aborted = true;
    
    buckets_warn("Streaming upload aborted: %s/%s", upload->bucket, upload->key);
}

/* ===================================================================
 * Streaming Handler Callbacks
 * ===================================================================*/

int s3_stream_on_request_start(uv_stream_request_t *req, void *user_data)
{
    (void)user_data;
    
    uv_http_conn_t *conn = req->conn;
    
    /* Only handle PUT requests */
    if (req->method != HTTP_PUT) {
        /* For non-PUT, let the legacy handler deal with it */
        return -1;
    }
    
    /* Parse URL */
    char bucket[256];
    char key[1024];
    
    if (parse_s3_url(req->url, bucket, sizeof(bucket), key, sizeof(key)) != 0) {
        send_error_response(conn, 400, "Invalid URL");
        return -1;
    }
    
    url_decode_inplace(bucket);
    url_decode_inplace(key);
    
    /* Must have both bucket and key for PUT object */
    if (bucket[0] == '\0' || key[0] == '\0') {
        /* This might be a bucket creation - let legacy handler deal with it */
        return -1;
    }
    
    /* Create upload state */
    s3_stream_upload_t *upload = s3_stream_upload_create(conn, bucket, key, 
                                                          req->content_length);
    if (!upload) {
        send_error_response(conn, 500, "Failed to create upload");
        return -1;
    }
    
    /* Get content type */
    const char *ct = get_header(conn, "Content-Type");
    if (ct) {
        strncpy(upload->content_type, ct, sizeof(upload->content_type) - 1);
    }
    
    /* Parse x-amz-meta-* headers using external iterator */
    extern int uv_http_iterate_headers_with_prefix(void *conn_ptr, const char *prefix,
                                                    void (*callback)(const char *name, const char *value, void *user_data),
                                                    void *user_data);
    
    uv_http_iterate_headers_with_prefix(conn, "x-amz-meta-", 
                                        stream_user_meta_callback, upload);
    
    if (upload->user_meta_count > 0) {
        buckets_info("Streaming PUT: parsed %d user metadata headers", upload->user_meta_count);
    }
    
    /* Store upload state in connection */
    conn->stream_ctx = upload;
    
    buckets_debug("Streaming PUT started: %s/%s", bucket, key);
    
    return 0;
}

int s3_stream_on_body_chunk(uv_stream_request_t *req,
                             const void *data, size_t len,
                             void *user_data)
{
    (void)user_data;
    
    s3_stream_upload_t *upload = (s3_stream_upload_t*)req->conn->stream_ctx;
    if (!upload) {
        return -1;
    }
    
    return s3_stream_upload_process(upload, data, len);
}

int s3_stream_on_request_complete(uv_stream_request_t *req, void *user_data)
{
    (void)user_data;
    
    s3_stream_upload_t *upload = (s3_stream_upload_t*)req->conn->stream_ctx;
    if (!upload) {
        return -1;
    }
    
    int ret = s3_stream_upload_complete(upload);
    
    if (ret == 0) {
        /* Send success response */
        send_put_success(req->conn, upload->etag);
    } else {
        send_error_response(req->conn, 500, "Upload failed");
    }
    
    /* Cleanup */
    s3_stream_upload_free(upload);
    req->conn->stream_ctx = NULL;
    
    return ret;
}

void s3_stream_on_request_error(uv_stream_request_t *req, int error, void *user_data)
{
    (void)user_data;
    (void)error;
    
    s3_stream_upload_t *upload = (s3_stream_upload_t*)req->conn->stream_ctx;
    if (upload) {
        s3_stream_upload_abort(upload);
        s3_stream_upload_free(upload);
        req->conn->stream_ctx = NULL;
    }
}

/* ===================================================================
 * Public API
 * ===================================================================*/

int s3_streaming_init(async_io_ctx_t *io_ctx)
{
    if (g_initialized) {
        return 0;
    }
    
    g_io_ctx = io_ctx;
    
    /* Setup handler */
    g_put_handler.on_request_start = s3_stream_on_request_start;
    g_put_handler.on_body_chunk = s3_stream_on_body_chunk;
    g_put_handler.on_request_complete = s3_stream_on_request_complete;
    g_put_handler.on_request_error = s3_stream_on_request_error;
    g_put_handler.user_data = NULL;
    
    g_initialized = true;
    
    buckets_info("S3 streaming handler initialized");
    
    return 0;
}

void s3_streaming_cleanup(void)
{
    g_io_ctx = NULL;
    g_initialized = false;
}

uv_stream_handler_t* s3_streaming_put_handler(void)
{
    return &g_put_handler;
}

int s3_streaming_init_default(void)
{
    /* Initialize with NULL io_ctx for now - will use sync I/O fallback */
    return s3_streaming_init(NULL);
}

/* ===================================================================
 * Legacy S3 Handler Wrapper (for non-streaming ops)
 * ===================================================================*/

/**
 * Wrapper that adapts UV connection to buckets_http_request/response format
 * and calls the existing S3 handler
 */
static void s3_legacy_uv_handler(uv_http_conn_t *conn, void *user_data)
{
    (void)user_data;
    
    /* Build buckets_http_request_t from connection */
    buckets_http_request_t http_req;
    memset(&http_req, 0, sizeof(http_req));
    
    http_req.method = llhttp_method_name(llhttp_get_method(&conn->parser));
    http_req.uri = conn->url;
    http_req.query_string = strchr(conn->url, '?');
    http_req.body = conn->body;
    http_req.body_len = conn->body_len;
    http_req.internal = conn;  /* For header access */
    
    /* Create response structure */
    buckets_http_response_t http_res;
    memset(&http_res, 0, sizeof(http_res));
    
    /* Call the S3 handler */
    extern void buckets_s3_handler(buckets_http_request_t *req,
                                    buckets_http_response_t *res,
                                    void *user_data);
    buckets_s3_handler(&http_req, &http_res, NULL);
    
    /* Send response using UV server API */
    if (http_res.status_code > 0) {
        /* Parse all headers from the headers string (format: "Name: value\r\n...") */
        /* Max 64 headers (32 name-value pairs) */
        const char *headers[64];
        int header_count = 0;
        
        /* Thread-local buffers for header names and values - allows up to 32 headers.
         * NOTE: These must NOT be static as this handler runs in libuv thread pool! */
        char header_names[32][128];
        char header_values[32][512];
        int parsed_count = 0;
        
        if (http_res.headers && http_res.headers[0] != '\0') {
            const char *p = http_res.headers;
            
            while (*p && parsed_count < 32) {
                /* Find the colon separating name from value */
                const char *colon = strchr(p, ':');
                if (!colon) break;
                
                /* Extract header name */
                size_t name_len = (size_t)(colon - p);
                if (name_len >= sizeof(header_names[0])) {
                    name_len = sizeof(header_names[0]) - 1;
                }
                strncpy(header_names[parsed_count], p, name_len);
                header_names[parsed_count][name_len] = '\0';
                
                /* Skip colon and optional space */
                const char *value_start = colon + 1;
                while (*value_start == ' ') value_start++;
                
                /* Find end of value (CRLF or end of string) */
                const char *value_end = strstr(value_start, "\r\n");
                if (!value_end) {
                    value_end = value_start + strlen(value_start);
                }
                
                /* Extract header value */
                size_t value_len = (size_t)(value_end - value_start);
                if (value_len >= sizeof(header_values[0])) {
                    value_len = sizeof(header_values[0]) - 1;
                }
                strncpy(header_values[parsed_count], value_start, value_len);
                header_values[parsed_count][value_len] = '\0';
                
                /* Add to headers array */
                headers[header_count++] = header_names[parsed_count];
                headers[header_count++] = header_values[parsed_count];
                parsed_count++;
                
                /* Move to next header */
                if (*value_end == '\r') {
                    p = value_end + 2;  /* Skip \r\n */
                } else {
                    break;
                }
            }
        }
        
        /* Default Content-Type if not set */
        if (parsed_count == 0 && http_res.body_len > 0) {
            headers[header_count++] = "Content-Type";
            headers[header_count++] = "application/xml";
        }
        
        headers[header_count] = NULL;
        
        uv_http_response_start(conn, http_res.status_code, headers, header_count, 
                               http_res.body_len);
        
        if (http_res.body && http_res.body_len > 0) {
            /* Write large responses in chunks to avoid blocking */
            const size_t CHUNK_SIZE = 64 * 1024;  /* 64KB chunks */
            size_t offset = 0;
            
            while (offset < http_res.body_len) {
                size_t remaining = http_res.body_len - offset;
                size_t write_size = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
                uv_http_response_write(conn, (char*)http_res.body + offset, write_size);
                offset += write_size;
            }
        }
        
        /* Mark response complete - critical for async handlers! */
        uv_http_response_end(conn);
    } else {
        /* No response set - send 500 */
        uv_http_response_start(conn, 500, NULL, 0, 0);
        uv_http_response_end(conn);
    }
    
    /* Free response body if allocated */
    if (http_res.body) {
        buckets_free(http_res.body);
    }
    if (http_res.headers) {
        buckets_free(http_res.headers);
    }
}

int s3_streaming_register_handlers(uv_http_server_t *server)
{
    if (!server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Ensure streaming is initialized */
    if (!g_initialized) {
        if (s3_streaming_init_default() != 0) {
            buckets_error("Failed to initialize streaming S3 subsystem");
            return BUCKETS_ERR_INIT;
        }
    }
    
    /* Register streaming PUT handler for all paths starting with / */
    int ret = uv_http_server_add_streaming_route(server, "PUT", "/", 
                                                   s3_streaming_put_handler());
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register streaming PUT handler");
        return ret;
    }
    
    buckets_info("Registered streaming S3 PUT handler");
    
    /* Register /rpc as SYNCHRONOUS route - runs in event loop.
     * RPC handlers (storage.deleteChunk, storage.readChunk, etc.) only do local
     * file I/O and DON'T make nested RPC calls. Running them in the event loop
     * ensures they can always respond immediately, preventing deadlock when
     * S3 operations are blocking the thread pool waiting for RPC responses. */
    ret = uv_http_server_add_route(server, "POST", "/rpc", 
                                    s3_legacy_uv_handler, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register RPC handler");
        return ret;
    }
    
    buckets_info("Registered SYNC RPC handler (runs in event loop for low latency)");
    
    /* Register /_internal/chunk as async route - binary transport also makes RPCs */
    ret = uv_http_server_add_async_route(server, "PUT", "/_internal/chunk",
                                          s3_legacy_uv_handler, NULL);
    if (ret != BUCKETS_OK) {
        buckets_warn("Failed to register async PUT chunk handler");
        /* Non-fatal - continue */
    }
    
    ret = uv_http_server_add_async_route(server, "GET", "/_internal/chunk",
                                          s3_legacy_uv_handler, NULL);
    if (ret != BUCKETS_OK) {
        buckets_warn("Failed to register async GET chunk handler");
        /* Non-fatal - continue */
    }
    
    /* Register legacy handler as ASYNC default for all S3 operations.
     * This is critical because S3 operations (PUT/GET/DELETE) make RPC calls
     * to other nodes. Running these in the event loop would block it. */
    ret = uv_http_server_set_async_handler(server, s3_legacy_uv_handler, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register async S3 handler");
        return ret;
    }
    
    buckets_info("Registered ASYNC S3 handler for GET/DELETE/HEAD/LIST (runs in thread pool)");
    
    return BUCKETS_OK;
}
