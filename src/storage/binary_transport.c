/**
 * Binary Chunk Transport
 * 
 * Provides efficient binary transport for chunk data between nodes.
 * Uses HTTP with binary body instead of base64-encoded JSON RPC.
 * 
 * Endpoints:
 *   PUT  /_internal/chunk - Write chunk to disk
 *   GET  /_internal/chunk - Read chunk from disk
 * 
 * Headers:
 *   X-Bucket: bucket name
 *   X-Object: object key (URL encoded)
 *   X-Chunk-Index: chunk index (1-based)
 *   X-Disk-Path: disk path
 *   Content-Length: chunk size (for writes)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_net.h"
#include "../net/uv_server_internal.h"

/* Buffer size for streaming */
#define STREAM_BUFFER_SIZE (256 * 1024)  /* 256 KB chunks */

/* Timeout for socket operations (seconds) */
#define SOCKET_TIMEOUT_SEC 300  /* 5 minutes for large transfers */

/* ===================================================================
 * URL Encoding/Decoding
 * ===================================================================*/

static char* url_encode(const char *str)
{
    if (!str) return NULL;
    
    size_t len = strlen(str);
    /* Worst case: every char needs encoding (3x size) */
    char *encoded = buckets_malloc(len * 3 + 1);
    if (!encoded) return NULL;
    
    char *p = encoded;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = c;
        } else {
            sprintf(p, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return encoded;
}

static char* url_decode(const char *str)
{
    if (!str) return NULL;
    
    size_t len = strlen(str);
    char *decoded = buckets_malloc(len + 1);
    if (!decoded) return NULL;
    
    char *p = decoded;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '%' && i + 2 < len) {
            unsigned int val;
            if (sscanf(str + i + 1, "%2x", &val) == 1) {
                *p++ = (char)val;
                i += 2;
            } else {
                *p++ = str[i];
            }
        } else if (str[i] == '+') {
            *p++ = ' ';
        } else {
            *p++ = str[i];
        }
    }
    *p = '\0';
    return decoded;
}

/* ===================================================================
 * Socket Helpers
 * ===================================================================*/

static int create_tcp_connection(const char *host, int port)
{
    struct addrinfo hints, *result, *rp;
    int fd = -1;
    char port_str[16];
    
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    int ret = getaddrinfo(host, port_str, &hints, &result);
    if (ret != 0) {
        buckets_error("getaddrinfo failed for %s:%d: %s", host, port, gai_strerror(ret));
        return -1;
    }
    
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == -1) continue;
        
        /* Set socket options */
        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        
        /* Set timeouts */
        struct timeval timeout;
        timeout.tv_sec = SOCKET_TIMEOUT_SEC;
        timeout.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  /* Success */
        }
        
        close(fd);
        fd = -1;
    }
    
    freeaddrinfo(result);
    
    if (fd == -1) {
        buckets_error("Failed to connect to %s:%d", host, port);
    }
    
    return fd;
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *ptr = (const char *)data;
    size_t remaining = len;
    
    while (remaining > 0) {
        ssize_t sent = send(fd, ptr, remaining, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            buckets_error("send failed: %s", strerror(errno));
            return -1;
        }
        if (sent == 0) {
            buckets_error("Connection closed during send");
            return -1;
        }
        ptr += sent;
        remaining -= sent;
    }
    
    return 0;
}

/* recv_all helper - currently unused but kept for future use */
static int recv_all(int fd, void *data, size_t len) __attribute__((unused));
static int recv_all(int fd, void *data, size_t len)
{
    char *ptr = (char *)data;
    size_t remaining = len;
    
    while (remaining > 0) {
        ssize_t received = recv(fd, ptr, remaining, 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            buckets_error("recv failed: %s", strerror(errno));
            return -1;
        }
        if (received == 0) {
            buckets_error("Connection closed during recv");
            return -1;
        }
        ptr += received;
        remaining -= received;
    }
    
    return 0;
}

/* Parse endpoint URL to extract host and port */
static int parse_endpoint(const char *endpoint, char *host, size_t host_len, int *port)
{
    /* Format: http://host:port or https://host:port */
    const char *proto_end = strstr(endpoint, "://");
    if (!proto_end) {
        return -1;
    }
    
    const char *host_start = proto_end + 3;
    const char *port_start = strchr(host_start, ':');
    if (!port_start) {
        /* No port specified, use default */
        const char *path_start = strchr(host_start, '/');
        size_t len = path_start ? (size_t)(path_start - host_start) : strlen(host_start);
        if (len >= host_len) return -1;
        memcpy(host, host_start, len);
        host[len] = '\0';
        *port = 80;  /* Default HTTP port */
    } else {
        size_t len = port_start - host_start;
        if (len >= host_len) return -1;
        memcpy(host, host_start, len);
        host[len] = '\0';
        *port = atoi(port_start + 1);
    }
    
    return 0;
}

/* ===================================================================
 * Binary Chunk Write (Client Side)
 * ===================================================================*/

/**
 * Write chunk to remote node using binary transport
 */
int buckets_binary_write_chunk(const char *peer_endpoint,
                                const char *bucket,
                                const char *object,
                                u32 chunk_index,
                                const void *chunk_data,
                                size_t chunk_size,
                                const char *disk_path)
{
    if (!peer_endpoint || !bucket || !object || !chunk_data || !disk_path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Parse endpoint */
    char host[256];
    int port;
    if (parse_endpoint(peer_endpoint, host, sizeof(host), &port) != 0) {
        buckets_error("Invalid endpoint: %s", peer_endpoint);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Connect to remote node */
    int fd = create_tcp_connection(host, port);
    if (fd < 0) {
        return BUCKETS_ERR_IO;
    }
    
    /* URL encode object key */
    char *encoded_object = url_encode(object);
    char *encoded_disk_path = url_encode(disk_path);
    
    if (!encoded_object || !encoded_disk_path) {
        if (encoded_object) buckets_free(encoded_object);
        if (encoded_disk_path) buckets_free(encoded_disk_path);
        close(fd);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Build HTTP request headers */
    char headers[2048];
    int header_len = snprintf(headers, sizeof(headers),
        "PUT /_internal/chunk HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %zu\r\n"
        "X-Bucket: %s\r\n"
        "X-Object: %s\r\n"
        "X-Chunk-Index: %u\r\n"
        "X-Disk-Path: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        host, port, chunk_size, bucket, encoded_object, chunk_index, encoded_disk_path);
    
    buckets_free(encoded_object);
    buckets_free(encoded_disk_path);
    
    /* Send headers */
    if (send_all(fd, headers, header_len) != 0) {
        close(fd);
        return BUCKETS_ERR_IO;
    }
    
    /* Stream chunk data in pieces */
    const char *data_ptr = (const char *)chunk_data;
    size_t remaining = chunk_size;
    size_t total_sent = 0;
    
    while (remaining > 0) {
        size_t to_send = remaining > STREAM_BUFFER_SIZE ? STREAM_BUFFER_SIZE : remaining;
        
        if (send_all(fd, data_ptr, to_send) != 0) {
            buckets_error("Failed to send chunk data (sent %zu/%zu)", total_sent, chunk_size);
            close(fd);
            return BUCKETS_ERR_IO;
        }
        
        data_ptr += to_send;
        remaining -= to_send;
        total_sent += to_send;
    }
    
    /* Read response headers */
    char response[1024];
    size_t resp_len = 0;
    char *headers_end = NULL;
    
    while (resp_len < sizeof(response) - 1) {
        ssize_t n = recv(fd, response + resp_len, sizeof(response) - resp_len - 1, 0);
        if (n <= 0) {
            buckets_error("Failed to receive response");
            close(fd);
            return BUCKETS_ERR_IO;
        }
        resp_len += n;
        response[resp_len] = '\0';
        
        headers_end = strstr(response, "\r\n\r\n");
        if (headers_end) break;
    }
    
    close(fd);
    
    /* Parse status code */
    int status_code = 0;
    if (sscanf(response, "HTTP/1.%*d %d", &status_code) != 1) {
        buckets_error("Failed to parse HTTP response");
        return BUCKETS_ERR_IO;
    }
    
    if (status_code != 200) {
        buckets_error("Remote chunk write failed with status %d", status_code);
        return BUCKETS_ERR_IO;
    }
    
    buckets_debug("Binary write: chunk %u to %s:%s (%zu bytes)", 
                  chunk_index, peer_endpoint, disk_path, chunk_size);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Binary Chunk Read (Client Side)
 * ===================================================================*/

/**
 * Read chunk from remote node using binary transport
 */
int buckets_binary_read_chunk(const char *peer_endpoint,
                               const char *bucket,
                               const char *object,
                               u32 chunk_index,
                               void **chunk_data,
                               size_t *chunk_size,
                               const char *disk_path)
{
    if (!peer_endpoint || !bucket || !object || !chunk_data || !chunk_size || !disk_path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    *chunk_data = NULL;
    *chunk_size = 0;
    
    /* Parse endpoint */
    char host[256];
    int port;
    if (parse_endpoint(peer_endpoint, host, sizeof(host), &port) != 0) {
        buckets_error("Invalid endpoint: %s", peer_endpoint);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Connect to remote node */
    int fd = create_tcp_connection(host, port);
    if (fd < 0) {
        return BUCKETS_ERR_IO;
    }
    
    /* URL encode object key */
    char *encoded_object = url_encode(object);
    char *encoded_disk_path = url_encode(disk_path);
    
    if (!encoded_object || !encoded_disk_path) {
        if (encoded_object) buckets_free(encoded_object);
        if (encoded_disk_path) buckets_free(encoded_disk_path);
        close(fd);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Build HTTP request */
    char request[2048];
    int req_len = snprintf(request, sizeof(request),
        "GET /_internal/chunk HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "X-Bucket: %s\r\n"
        "X-Object: %s\r\n"
        "X-Chunk-Index: %u\r\n"
        "X-Disk-Path: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        host, port, bucket, encoded_object, chunk_index, encoded_disk_path);
    
    buckets_free(encoded_object);
    buckets_free(encoded_disk_path);
    
    /* Send request */
    if (send_all(fd, request, req_len) != 0) {
        close(fd);
        return BUCKETS_ERR_IO;
    }
    
    /* Read response headers */
    char header_buf[4096];
    size_t header_len = 0;
    char *headers_end = NULL;
    
    while (header_len < sizeof(header_buf) - 1) {
        ssize_t n = recv(fd, header_buf + header_len, sizeof(header_buf) - header_len - 1, 0);
        if (n <= 0) {
            buckets_error("Failed to receive response headers");
            close(fd);
            return BUCKETS_ERR_IO;
        }
        header_len += n;
        header_buf[header_len] = '\0';
        
        headers_end = strstr(header_buf, "\r\n\r\n");
        if (headers_end) break;
    }
    
    if (!headers_end) {
        buckets_error("Failed to find end of headers");
        close(fd);
        return BUCKETS_ERR_IO;
    }
    
    /* Parse status code */
    int status_code = 0;
    if (sscanf(header_buf, "HTTP/1.%*d %d", &status_code) != 1) {
        buckets_error("Failed to parse HTTP response");
        close(fd);
        return BUCKETS_ERR_IO;
    }
    
    if (status_code != 200) {
        buckets_error("Remote chunk read failed with status %d", status_code);
        close(fd);
        return BUCKETS_ERR_IO;
    }
    
    /* Parse Content-Length */
    size_t content_length = 0;
    char *cl = strstr(header_buf, "Content-Length:");
    if (!cl) {
        cl = strstr(header_buf, "content-length:");
    }
    if (cl) {
        sscanf(cl, "%*[^:]: %zu", &content_length);
    }
    
    if (content_length == 0) {
        buckets_error("Missing or invalid Content-Length");
        close(fd);
        return BUCKETS_ERR_IO;
    }
    
    /* Allocate buffer for chunk data */
    void *data = buckets_malloc(content_length);
    if (!data) {
        close(fd);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Calculate how much body data we already received with headers */
    size_t body_start = (headers_end + 4) - header_buf;
    size_t body_in_buffer = header_len - body_start;
    
    if (body_in_buffer > 0) {
        memcpy(data, headers_end + 4, body_in_buffer);
    }
    
    /* Read remaining body data */
    size_t remaining = content_length - body_in_buffer;
    char *write_ptr = (char*)data + body_in_buffer;
    
    while (remaining > 0) {
        ssize_t n = recv(fd, write_ptr, remaining, 0);
        if (n <= 0) {
            buckets_error("Failed to receive chunk data");
            buckets_free(data);
            close(fd);
            return BUCKETS_ERR_IO;
        }
        write_ptr += n;
        remaining -= n;
    }
    
    close(fd);
    
    *chunk_data = data;
    *chunk_size = content_length;
    
    buckets_debug("Binary read: chunk %u from %s:%s (%zu bytes)",
                  chunk_index, peer_endpoint, disk_path, content_length);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Binary Chunk Handlers (Server Side)
 * ===================================================================*/

/**
 * HTTP handler for PUT /_internal/chunk
 */
void buckets_binary_chunk_write_handler(buckets_http_request_t *req,
                                         buckets_http_response_t *res,
                                         void *user_data)
{
    (void)user_data;
    
    /* Get metadata from headers - req->internal is uv_http_conn_t */
    uv_http_conn_t *conn = (uv_http_conn_t *)req->internal;
    
    const char *bucket_hdr = uv_http_get_header(conn, "X-Bucket");
    const char *object_hdr = uv_http_get_header(conn, "X-Object");
    const char *chunk_idx_hdr = uv_http_get_header(conn, "X-Chunk-Index");
    const char *disk_path_hdr = uv_http_get_header(conn, "X-Disk-Path");
    
    if (bucket_hdr == NULL || object_hdr == NULL || 
        chunk_idx_hdr == NULL || disk_path_hdr == NULL) {
        res->status_code = 400;
        res->body = buckets_strdup("Missing required headers");
        res->body_len = strlen(res->body);
        return;
    }
    
    /* Extract header values (UV returns null-terminated strings) */
    char bucket[256], object_encoded[1024], disk_path_encoded[512];
    char chunk_idx_str[32];
    
    strncpy(bucket, bucket_hdr, sizeof(bucket) - 1);
    strncpy(object_encoded, object_hdr, sizeof(object_encoded) - 1);
    strncpy(chunk_idx_str, chunk_idx_hdr, sizeof(chunk_idx_str) - 1);
    strncpy(disk_path_encoded, disk_path_hdr, sizeof(disk_path_encoded) - 1);
    
    /* URL decode */
    char *object = url_decode(object_encoded);
    char *disk_path = url_decode(disk_path_encoded);
    u32 chunk_index = (u32)atoi(chunk_idx_str);
    
    if (!object || !disk_path) {
        if (object) buckets_free(object);
        if (disk_path) buckets_free(disk_path);
        res->status_code = 400;
        res->body = buckets_strdup("Failed to decode headers");
        res->body_len = strlen(res->body);
        return;
    }
    
    buckets_debug("Binary chunk write: %s/%s chunk %u -> %s (%zu bytes)",
                  bucket, object, chunk_index, disk_path, req->body_len);
    
    /* Compute object path */
    char object_path[PATH_MAX];
    extern void buckets_compute_object_path(const char *bucket, const char *object,
                                            char *path, size_t path_len);
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Create object directory if needed */
    extern int buckets_create_object_dir(const char *disk_path, const char *object_path);
    if (buckets_create_object_dir(disk_path, object_path) != 0) {
        buckets_free(object);
        buckets_free(disk_path);
        res->status_code = 500;
        res->body = buckets_strdup("Failed to create directory");
        res->body_len = strlen(res->body);
        return;
    }
    
    /* Write chunk to disk */
    extern int buckets_write_chunk(const char *disk_path, const char *object_path,
                                   u32 chunk_index, const void *data, size_t size);
    
    int ret = buckets_write_chunk(disk_path, object_path, chunk_index, 
                                   req->body, req->body_len);
    
    buckets_free(object);
    buckets_free(disk_path);
    
    if (ret != 0) {
        res->status_code = 500;
        res->body = buckets_strdup("Failed to write chunk");
        res->body_len = strlen(res->body);
        return;
    }
    
    res->status_code = 200;
    res->body = buckets_strdup("OK");
    res->body_len = 2;
}

/**
 * HTTP handler for GET /_internal/chunk
 */
void buckets_binary_chunk_read_handler(buckets_http_request_t *req,
                                        buckets_http_response_t *res,
                                        void *user_data)
{
    (void)user_data;
    
    /* Get metadata from headers - req->internal is uv_http_conn_t */
    uv_http_conn_t *conn = (uv_http_conn_t *)req->internal;
    
    const char *bucket_hdr = uv_http_get_header(conn, "X-Bucket");
    const char *object_hdr = uv_http_get_header(conn, "X-Object");
    const char *chunk_idx_hdr = uv_http_get_header(conn, "X-Chunk-Index");
    const char *disk_path_hdr = uv_http_get_header(conn, "X-Disk-Path");
    
    if (bucket_hdr == NULL || object_hdr == NULL || 
        chunk_idx_hdr == NULL || disk_path_hdr == NULL) {
        res->status_code = 400;
        res->body = buckets_strdup("Missing required headers");
        res->body_len = strlen(res->body);
        return;
    }
    
    /* Extract header values (UV returns null-terminated strings) */
    char bucket[256], object_encoded[1024], disk_path_encoded[512];
    char chunk_idx_str[32];
    
    strncpy(bucket, bucket_hdr, sizeof(bucket) - 1);
    strncpy(object_encoded, object_hdr, sizeof(object_encoded) - 1);
    strncpy(chunk_idx_str, chunk_idx_hdr, sizeof(chunk_idx_str) - 1);
    strncpy(disk_path_encoded, disk_path_hdr, sizeof(disk_path_encoded) - 1);
    
    /* URL decode */
    char *object = url_decode(object_encoded);
    char *disk_path = url_decode(disk_path_encoded);
    u32 chunk_index = (u32)atoi(chunk_idx_str);
    
    if (!object || !disk_path) {
        if (object) buckets_free(object);
        if (disk_path) buckets_free(disk_path);
        res->status_code = 400;
        res->body = buckets_strdup("Failed to decode headers");
        res->body_len = strlen(res->body);
        return;
    }
    
    /* Compute object path */
    char object_path[PATH_MAX];
    extern void buckets_compute_object_path(const char *bucket, const char *object,
                                            char *path, size_t path_len);
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Read chunk from disk */
    extern int buckets_read_chunk(const char *disk_path, const char *object_path,
                                  u32 chunk_index, void **data, size_t *size);
    
    void *chunk_data = NULL;
    size_t chunk_size = 0;
    
    int ret = buckets_read_chunk(disk_path, object_path, chunk_index,
                                  &chunk_data, &chunk_size);
    
    buckets_free(object);
    buckets_free(disk_path);
    
    if (ret != 0 || !chunk_data) {
        res->status_code = 404;
        res->body = buckets_strdup("Chunk not found");
        res->body_len = strlen(res->body);
        return;
    }
    
    /* Return chunk data as binary body */
    res->status_code = 200;
    res->body = chunk_data;
    res->body_len = chunk_size;
    
    buckets_debug("Binary chunk read: chunk %u (%zu bytes)", chunk_index, chunk_size);
}

/* ===================================================================
 * Registration
 * ===================================================================*/

/**
 * Register binary chunk transport handlers with HTTP server
 */
int buckets_binary_transport_register(buckets_http_server_t *server)
{
    if (!server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    buckets_router_t *router = buckets_http_server_get_router(server);
    if (!router) {
        buckets_error("No router available for binary transport registration");
        return BUCKETS_ERR_INIT;
    }
    
    /* Register PUT handler for chunk writes */
    int ret = buckets_router_add_route(router, "PUT", "/_internal/chunk",
                                        buckets_binary_chunk_write_handler, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register binary chunk write handler");
        return ret;
    }
    
    /* Register GET handler for chunk reads */
    ret = buckets_router_add_route(router, "GET", "/_internal/chunk",
                                    buckets_binary_chunk_read_handler, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register binary chunk read handler");
        return ret;
    }
    
    buckets_info("Binary chunk transport handlers registered");
    
    return BUCKETS_OK;
}
