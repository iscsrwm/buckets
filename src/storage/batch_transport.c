/**
 * Batched Binary Chunk Transport
 * 
 * Optimizes distributed chunk writes by batching multiple chunks destined
 * for the same node into a single HTTP request. Reduces network round-trips
 * and improves throughput for erasure-coded objects.
 * 
 * Endpoint:
 *   PUT  /_internal/batch_chunks - Write multiple chunks in one request
 * 
 * Format (multipart-like):
 *   Content-Type: application/x-buckets-batch
 *   X-Batch-Count: N
 *   
 *   Body format (binary):
 *     For each chunk:
 *       [4 bytes: chunk_index (u32, network order)]
 *       [4 bytes: chunk_size (u32, network order)]
 *       [256 bytes: bucket (null-terminated string)]
 *       [1024 bytes: object (null-terminated string)]
 *       [512 bytes: disk_path (null-terminated string)]
 *       [chunk_size bytes: chunk data]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_net.h"
#include "buckets_debug.h"
#include "../net/uv_server_internal.h"  /* For uv_http_request_t and uv_http_response_t */

/* External debug stats */
extern buckets_debug_stats_t g_stats;

/* Fixed-size metadata fields to simplify parsing */
#define BATCH_BUCKET_SIZE  256
#define BATCH_OBJECT_SIZE  1024
#define BATCH_DISKPATH_SIZE 512
#define BATCH_HEADER_SIZE  (8 + BATCH_BUCKET_SIZE + BATCH_OBJECT_SIZE + BATCH_DISKPATH_SIZE)

/* Socket timeout */
#define SOCKET_TIMEOUT_SEC 30

/* External connection cache functions from binary_transport.c */
extern int get_cached_connection(const char *host, int port);
extern void cache_connection(int fd, const char *host, int port);
extern void close_tcp_connection(int fd);
extern int create_tcp_connection(const char *host, int port);
extern int parse_endpoint(const char *endpoint, char *host, size_t host_len, int *port);

/* Note: batch_chunk_t is now buckets_batch_chunk_t from buckets_storage.h */

/* send_all() removed - now using writev() for zero-copy transmission */

/**
 * Write multiple chunks to remote node in a single HTTP request
 * 
 * @param peer_endpoint Remote node endpoint (e.g., "http://localhost:9001")
 * @param chunks Array of chunks to write
 * @param chunk_count Number of chunks in array
 * @return 0 on success, negative error code on failure
 */
int buckets_binary_batch_write_chunks(const char *peer_endpoint,
                                       const buckets_batch_chunk_t *chunks,
                                       size_t chunk_count)
{
    if (!peer_endpoint || !chunks || chunk_count == 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    struct timespec start_time;
    if (g_debug_instrumentation_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &start_time);
    }
    
    DEBUG_INC(g_stats.binary_writes_total);
    DEBUG_INC(g_stats.binary_writes_active);
    
    /* Parse endpoint */
    char host[256];
    int port;
    if (parse_endpoint(peer_endpoint, host, sizeof(host), &port) != 0) {
        buckets_error("Invalid endpoint: %s", peer_endpoint);
        DEBUG_DEC(g_stats.binary_writes_active);
        DEBUG_INC(g_stats.binary_writes_failed);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    buckets_debug("[BATCH_WRITE] endpoint=%s count=%zu", peer_endpoint, chunk_count);
    
    /* Calculate total body size */
    size_t total_body_size = 0;
    for (size_t i = 0; i < chunk_count; i++) {
        total_body_size += BATCH_HEADER_SIZE + chunks[i].chunk_size;
    }
    
    /* Try to get cached connection */
    int fd = get_cached_connection(host, port);
    
    if (fd < 0) {
        fd = create_tcp_connection(host, port);
        if (fd < 0) {
            buckets_error("[BATCH_WRITE] Connection failed to %s:%d", host, port);
            DEBUG_DEC(g_stats.binary_writes_active);
            DEBUG_INC(g_stats.binary_writes_failed);
            return BUCKETS_ERR_IO;
        }
    }
    
    /* Build HTTP request headers */
    char headers[1024];
    int header_len = snprintf(headers, sizeof(headers),
        "PUT /_internal/batch_chunks HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/x-buckets-batch\r\n"
        "Content-Length: %zu\r\n"
        "X-Batch-Count: %zu\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        host, port, total_body_size, chunk_count);
    
    /* ZERO-COPY OPTIMIZATION: Use writev to send all data in one syscall without copying */
    
    /* Allocate chunk headers (will be sent with writev, no extra buffer needed) */
    char *chunk_headers = buckets_malloc(chunk_count * BATCH_HEADER_SIZE);
    if (!chunk_headers) {
        buckets_error("[BATCH_WRITE] Failed to allocate chunk headers");
        close_tcp_connection(fd);
        DEBUG_DEC(g_stats.binary_writes_active);
        DEBUG_INC(g_stats.binary_writes_failed);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Prepare chunk headers */
    for (size_t i = 0; i < chunk_count; i++) {
        const buckets_batch_chunk_t *chunk = &chunks[i];
        char *chunk_header = chunk_headers + (i * BATCH_HEADER_SIZE);
        memset(chunk_header, 0, BATCH_HEADER_SIZE);
        
        /* Network byte order for integers */
        u32 chunk_index_net = htonl(chunk->chunk_index);
        u32 chunk_size_net = htonl((u32)chunk->chunk_size);
        
        /* Pack header */
        char *p = chunk_header;
        memcpy(p, &chunk_index_net, 4); p += 4;
        memcpy(p, &chunk_size_net, 4); p += 4;
        strncpy(p, chunk->bucket, BATCH_BUCKET_SIZE - 1); p += BATCH_BUCKET_SIZE;
        strncpy(p, chunk->object, BATCH_OBJECT_SIZE - 1); p += BATCH_OBJECT_SIZE;
        strncpy(p, chunk->disk_path, BATCH_DISKPATH_SIZE - 1);
    }
    
    /* Build iovec array: HTTP headers + (chunk_header + chunk_data) for each chunk */
    size_t iov_count = 1 + (chunk_count * 2);  /* 1 HTTP header + 2 per chunk (header + data) */
    struct iovec *iov = buckets_malloc(iov_count * sizeof(struct iovec));
    if (!iov) {
        buckets_error("[BATCH_WRITE] Failed to allocate iovec array");
        buckets_free(chunk_headers);
        close_tcp_connection(fd);
        DEBUG_DEC(g_stats.binary_writes_active);
        DEBUG_INC(g_stats.binary_writes_failed);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Fill iovec array */
    size_t iov_idx = 0;
    
    /* HTTP headers */
    iov[iov_idx].iov_base = headers;
    iov[iov_idx].iov_len = header_len;
    iov_idx++;
    
    /* Each chunk: header + data */
    for (size_t i = 0; i < chunk_count; i++) {
        /* Chunk header */
        iov[iov_idx].iov_base = chunk_headers + (i * BATCH_HEADER_SIZE);
        iov[iov_idx].iov_len = BATCH_HEADER_SIZE;
        iov_idx++;
        
        /* Chunk data (ZERO-COPY: points directly to original buffer) */
        iov[iov_idx].iov_base = (void*)chunks[i].chunk_data;
        iov[iov_idx].iov_len = chunks[i].chunk_size;
        iov_idx++;
    }
    
    /* Send everything in one syscall with writev (scatter-gather I/O) */
    size_t total_to_send = 0;
    for (size_t i = 0; i < iov_count; i++) {
        total_to_send += iov[i].iov_len;
    }
    
    size_t total_sent = 0;
    while (total_sent < total_to_send) {
        ssize_t n = writev(fd, iov, iov_count);
        if (n < 0) {
            if (errno == EINTR) continue;
            buckets_error("[BATCH_WRITE] writev failed: %s", strerror(errno));
            buckets_free(chunk_headers);
            buckets_free(iov);
            close_tcp_connection(fd);
            DEBUG_DEC(g_stats.binary_writes_active);
            DEBUG_INC(g_stats.binary_writes_failed);
            return BUCKETS_ERR_IO;
        }
        
        total_sent += n;
        
        /* If partial write, adjust iov array for next writev call */
        if (total_sent < total_to_send) {
            size_t consumed = n;
            for (size_t i = 0; i < iov_count && consumed > 0; i++) {
                if (consumed >= iov[i].iov_len) {
                    consumed -= iov[i].iov_len;
                    iov[i].iov_len = 0;
                } else {
                    iov[i].iov_base = (char*)iov[i].iov_base + consumed;
                    iov[i].iov_len -= consumed;
                    consumed = 0;
                }
            }
        }
    }
    
    buckets_free(chunk_headers);
    buckets_free(iov);
    
    buckets_debug("[BATCH_WRITE] Sent %zu chunks (%zu bytes total) with zero-copy writev", 
                  chunk_count, total_to_send);
    
    /* Read response */
    char response[1024];
    size_t resp_len = 0;
    char *headers_end = NULL;
    
    while (resp_len < sizeof(response) - 1) {
        ssize_t n = recv(fd, response + resp_len, sizeof(response) - resp_len - 1, 0);
        if (n <= 0) {
            buckets_error("[BATCH_WRITE] Failed to receive response");
            close_tcp_connection(fd);
            DEBUG_DEC(g_stats.binary_writes_active);
            DEBUG_INC(g_stats.binary_writes_failed);
            return BUCKETS_ERR_IO;
        }
        resp_len += n;
        response[resp_len] = '\0';
        
        headers_end = strstr(response, "\r\n\r\n");
        if (headers_end) break;
    }
    
    /* Parse status code */
    int status_code = 0;
    if (sscanf(response, "HTTP/1.%*d %d", &status_code) != 1) {
        buckets_error("[BATCH_WRITE] Failed to parse HTTP response");
        close_tcp_connection(fd);
        DEBUG_DEC(g_stats.binary_writes_active);
        DEBUG_INC(g_stats.binary_writes_failed);
        return BUCKETS_ERR_IO;
    }
    
    if (status_code != 200) {
        buckets_error("[BATCH_WRITE] Remote batch write failed with status %d", status_code);
        close_tcp_connection(fd);
        DEBUG_DEC(g_stats.binary_writes_active);
        DEBUG_INC(g_stats.binary_writes_failed);
        return BUCKETS_ERR_IO;
    }
    
    /* Always cache connection for reuse (whether it was cached before or not) */
    cache_connection(fd, host, port);
    
    if (g_debug_instrumentation_enabled) {
        struct timespec end_time;
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        double total_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                         (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
        
        size_t total_bytes = 0;
        for (size_t i = 0; i < chunk_count; i++) {
            total_bytes += chunks[i].chunk_size;
        }
        
        buckets_info("[BATCH_WRITE] SUCCESS: %zu chunks, %zu bytes in %.2f ms (%.2f MB/s) endpoint=%s",
                     chunk_count, total_bytes, total_ms,
                     (total_bytes / 1024.0 / 1024.0) / (total_ms / 1000.0),
                     peer_endpoint);
    }
    
    DEBUG_DEC(g_stats.binary_writes_active);
    
    return BUCKETS_OK;
}

/**
 * Server-side handler for batch chunk writes
 * Called by HTTP server when receiving PUT /_internal/batch_chunks
 */
void buckets_handle_batch_chunk_write(buckets_http_request_t *req,
                                       buckets_http_response_t *res,
                                       void *user_data)
{
    (void)user_data;
    
    /* Get metadata from headers - req->internal is uv_http_conn_t */
    uv_http_conn_t *conn = (uv_http_conn_t *)req->internal;
    
    /* Get batch count from header */
    const char *count_header = uv_http_get_header(conn, "X-Batch-Count");
    if (!count_header) {
        buckets_error("[BATCH_HANDLER] Missing X-Batch-Count header");
        res->status_code = 400;
        res->body = buckets_strdup("Missing X-Batch-Count header\n");
        res->body_len = strlen(res->body);
        return;
    }
    
    size_t chunk_count = (size_t)atoi(count_header);
    if (chunk_count == 0 || chunk_count > 32) {
        buckets_error("[BATCH_HANDLER] Invalid chunk count: %zu", chunk_count);
        res->status_code = 400;
        res->body = buckets_strdup("Invalid chunk count\n");
        res->body_len = strlen(res->body);
        return;
    }
    
    buckets_debug("[BATCH_HANDLER] Receiving %zu chunks", chunk_count);
    
    /* Get request body */
    const char *body = req->body;
    size_t body_len = req->body_len;
    size_t offset = 0;
    size_t chunks_written = 0;
    
    for (size_t i = 0; i < chunk_count; i++) {
        /* Ensure we have enough data for header */
        if (offset + BATCH_HEADER_SIZE > body_len) {
            buckets_error("[BATCH_HANDLER] Truncated chunk header at offset %zu", offset);
            res->status_code = 400;
            res->body = buckets_strdup("Truncated chunk header\n");
            res->body_len = strlen(res->body);
            return;
        }
        
        /* Parse chunk header */
        const char *p = body + offset;
        u32 chunk_index_net, chunk_size_net;
        memcpy(&chunk_index_net, p, 4); p += 4;
        memcpy(&chunk_size_net, p, 4); p += 4;
        
        u32 chunk_index = ntohl(chunk_index_net);
        u32 chunk_size = ntohl(chunk_size_net);
        
        char bucket[BATCH_BUCKET_SIZE];
        char object[BATCH_OBJECT_SIZE];
        char disk_path[BATCH_DISKPATH_SIZE];
        
        strncpy(bucket, p, BATCH_BUCKET_SIZE - 1); bucket[BATCH_BUCKET_SIZE - 1] = '\0'; p += BATCH_BUCKET_SIZE;
        strncpy(object, p, BATCH_OBJECT_SIZE - 1); object[BATCH_OBJECT_SIZE - 1] = '\0'; p += BATCH_OBJECT_SIZE;
        strncpy(disk_path, p, BATCH_DISKPATH_SIZE - 1); disk_path[BATCH_DISKPATH_SIZE - 1] = '\0';
        
        offset += BATCH_HEADER_SIZE;
        
        /* Ensure we have chunk data */
        if (offset + chunk_size > body_len) {
            buckets_error("[BATCH_HANDLER] Truncated chunk data at offset %zu", offset);
            res->status_code = 400;
            res->body = buckets_strdup("Truncated chunk data\n");
            res->body_len = strlen(res->body);
            return;
        }
        
        const void *chunk_data = body + offset;
        offset += chunk_size;
        
        buckets_debug("[BATCH_HANDLER] chunk=%u size=%u bucket=%s object=%s disk=%s",
                      chunk_index, chunk_size, bucket, object, disk_path);
        
        /* Compute object path hash */
        char object_path[1536];
        extern void buckets_compute_object_path(const char *bucket, const char *object,
                                                char *object_path, size_t path_len);
        buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
        
        /* Write chunk to disk */
        extern int buckets_write_chunk(const char *disk_path, const char *object_path,
                                      u32 chunk_index, const void *data, size_t size);
        
        int ret = buckets_write_chunk(disk_path, object_path, chunk_index, chunk_data, chunk_size);
        if (ret != 0) {
            buckets_error("[BATCH_HANDLER] Failed to write chunk %u", chunk_index);
            continue;  /* Try other chunks */
        }
        
        chunks_written++;
        buckets_debug("[BATCH_HANDLER] Wrote chunk %u successfully", chunk_index);
    }
    
    buckets_info("[BATCH_HANDLER] Wrote %zu/%zu chunks successfully", chunks_written, chunk_count);
    
    /* Return success if at least quorum chunks were written */
    if (chunks_written >= (chunk_count / 2 + 1)) {
        res->status_code = 200;
        res->body = buckets_strdup("OK\n");
        res->body_len = 3;
    } else {
        res->status_code = 500;
        res->body = buckets_strdup("Insufficient chunks written\n");
        res->body_len = strlen(res->body);
    }
}
