/**
 * UV HTTP Server Implementation
 * 
 * High-performance HTTP server using libuv for async I/O and llhttp for parsing.
 * Supports streaming request bodies, TLS, and keep-alive connections.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "buckets.h"
#include "buckets_net.h"
#include "uv_server_internal.h"

/* ===================================================================
 * Forward Declarations
 * ===================================================================*/

static void on_connection(uv_stream_t *server, int status);
static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void on_write_complete(uv_write_t *req, int status);
static void on_handle_close(uv_handle_t *handle);
static void on_timeout(uv_timer_t *timer);
static void on_shutdown_async(uv_async_t *handle);
static void* server_thread_main(void *arg);

static void process_request(uv_http_conn_t *conn);
static void setup_parser_callbacks(llhttp_settings_t *settings);

/* Streaming handler helpers */
static uv_route_t* find_streaming_route(uv_http_conn_t *conn);
static void build_stream_request(uv_http_conn_t *conn, uv_stream_request_t *req);

/* ===================================================================
 * HTTP Status Codes
 * ===================================================================*/

static const char* http_status_string(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

/* ===================================================================
 * Server Creation and Lifecycle
 * ===================================================================*/

uv_http_server_t* uv_http_server_create(const char *addr, int port)
{
    if (!addr || port <= 0 || port > 65535) {
        buckets_error("Invalid address or port");
        return NULL;
    }
    
    uv_http_server_t *server = buckets_calloc(1, sizeof(uv_http_server_t));
    if (!server) {
        return NULL;
    }
    
    strncpy(server->address, addr, sizeof(server->address) - 1);
    server->port = port;
    server->backlog = BUCKETS_DEFAULT_BACKLOG;
    server->max_connections = BUCKETS_DEFAULT_MAX_CONNECTIONS;
    
    /* Set default timeouts */
    server->headers_timeout_ms = BUCKETS_DEFAULT_HEADERS_TIMEOUT_MS;
    server->body_timeout_ms = BUCKETS_DEFAULT_BODY_TIMEOUT_MS;
    server->keepalive_timeout_ms = BUCKETS_DEFAULT_KEEPALIVE_TIMEOUT_MS;
    server->write_timeout_ms = BUCKETS_DEFAULT_WRITE_TIMEOUT_MS;
    
    pthread_mutex_init(&server->lock, NULL);
    
    buckets_info("Created UV HTTP server for %s:%d", addr, port);
    
    return server;
}

int uv_http_server_enable_tls(uv_http_server_t *server,
                               const char *cert_file,
                               const char *key_file)
{
    if (!server || !cert_file || !key_file) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    strncpy(server->cert_file, cert_file, sizeof(server->cert_file) - 1);
    strncpy(server->key_file, key_file, sizeof(server->key_file) - 1);
    server->tls_enabled = true;
    
    return BUCKETS_OK;
}

int uv_http_server_set_handler(uv_http_server_t *server,
                                uv_http_handler_t handler,
                                void *user_data)
{
    if (!server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&server->lock);
    server->default_handler = handler;
    server->default_handler_data = user_data;
    pthread_mutex_unlock(&server->lock);
    
    return BUCKETS_OK;
}

int uv_http_server_add_route(uv_http_server_t *server,
                              const char *method,
                              const char *path_prefix,
                              uv_http_handler_t handler,
                              void *user_data)
{
    if (!server || !path_prefix || !handler) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    uv_route_t *route = buckets_calloc(1, sizeof(uv_route_t));
    if (!route) {
        return BUCKETS_ERR_NOMEM;
    }
    
    if (method) {
        route->method = buckets_strdup(method);
    }
    route->path_prefix = buckets_strdup(path_prefix);
    route->is_streaming = false;
    route->handler.legacy = handler;
    route->user_data = user_data;
    
    pthread_mutex_lock(&server->lock);
    route->next = server->routes;
    server->routes = route;
    pthread_mutex_unlock(&server->lock);
    
    return BUCKETS_OK;
}

int uv_http_server_add_streaming_route(uv_http_server_t *server,
                                        const char *method,
                                        const char *path_prefix,
                                        uv_stream_handler_t *handler)
{
    if (!server || !path_prefix || !handler) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    uv_route_t *route = buckets_calloc(1, sizeof(uv_route_t));
    if (!route) {
        return BUCKETS_ERR_NOMEM;
    }
    
    if (method) {
        route->method = buckets_strdup(method);
    }
    route->path_prefix = buckets_strdup(path_prefix);
    route->is_streaming = true;
    route->handler.streaming = *handler;
    route->user_data = handler->user_data;
    
    pthread_mutex_lock(&server->lock);
    route->next = server->routes;
    server->routes = route;
    pthread_mutex_unlock(&server->lock);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * TLS Initialization
 * ===================================================================*/

int uv_http_tls_init(uv_http_server_t *server)
{
    if (!server->tls_enabled) {
        return BUCKETS_OK;
    }
    
    /* Initialize OpenSSL */
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();
    
    /* Create SSL context */
    const SSL_METHOD *method = TLS_server_method();
    server->ssl_ctx = SSL_CTX_new(method);
    if (!server->ssl_ctx) {
        buckets_error("Failed to create SSL context");
        return BUCKETS_ERR_IO;
    }
    
    /* Set minimum TLS version to 1.2 */
    SSL_CTX_set_min_proto_version(server->ssl_ctx, TLS1_2_VERSION);
    
    /* Load certificate */
    if (SSL_CTX_use_certificate_file(server->ssl_ctx, server->cert_file, 
                                      SSL_FILETYPE_PEM) <= 0) {
        buckets_error("Failed to load certificate: %s", server->cert_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(server->ssl_ctx);
        server->ssl_ctx = NULL;
        return BUCKETS_ERR_IO;
    }
    
    /* Load private key */
    if (SSL_CTX_use_PrivateKey_file(server->ssl_ctx, server->key_file,
                                     SSL_FILETYPE_PEM) <= 0) {
        buckets_error("Failed to load private key: %s", server->key_file);
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(server->ssl_ctx);
        server->ssl_ctx = NULL;
        return BUCKETS_ERR_IO;
    }
    
    /* Verify private key matches certificate */
    if (!SSL_CTX_check_private_key(server->ssl_ctx)) {
        buckets_error("Private key does not match certificate");
        SSL_CTX_free(server->ssl_ctx);
        server->ssl_ctx = NULL;
        return BUCKETS_ERR_IO;
    }
    
    buckets_info("TLS initialized with certificate: %s", server->cert_file);
    
    return BUCKETS_OK;
}

void uv_http_tls_cleanup(uv_http_server_t *server)
{
    if (server->ssl_ctx) {
        SSL_CTX_free(server->ssl_ctx);
        server->ssl_ctx = NULL;
    }
}

/* ===================================================================
 * Server Start/Stop
 * ===================================================================*/

int uv_http_server_start(uv_http_server_t *server)
{
    if (!server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (server->running) {
        buckets_warn("Server already running");
        return BUCKETS_OK;
    }
    
    /* Initialize TLS if enabled */
    int ret = uv_http_tls_init(server);
    if (ret != BUCKETS_OK) {
        return ret;
    }
    
    /* Create event loop */
    server->loop = buckets_malloc(sizeof(uv_loop_t));
    if (!server->loop) {
        return BUCKETS_ERR_NOMEM;
    }
    
    ret = uv_loop_init(server->loop);
    if (ret != 0) {
        buckets_error("Failed to init event loop: %s", uv_strerror(ret));
        buckets_free(server->loop);
        server->loop = NULL;
        return BUCKETS_ERR_IO;
    }
    server->owns_loop = true;
    
    /* Initialize TCP handle */
    ret = uv_tcp_init(server->loop, &server->tcp);
    if (ret != 0) {
        buckets_error("Failed to init TCP: %s", uv_strerror(ret));
        uv_loop_close(server->loop);
        buckets_free(server->loop);
        server->loop = NULL;
        return BUCKETS_ERR_IO;
    }
    server->tcp.data = server;
    
    /* Bind to address */
    struct sockaddr_in addr;
    ret = uv_ip4_addr(server->address, server->port, &addr);
    if (ret != 0) {
        buckets_error("Invalid address: %s", uv_strerror(ret));
        uv_close((uv_handle_t*)&server->tcp, NULL);
        uv_loop_close(server->loop);
        buckets_free(server->loop);
        server->loop = NULL;
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    ret = uv_tcp_bind(&server->tcp, (struct sockaddr*)&addr, 0);
    if (ret != 0) {
        buckets_error("Failed to bind: %s", uv_strerror(ret));
        uv_close((uv_handle_t*)&server->tcp, NULL);
        uv_loop_close(server->loop);
        buckets_free(server->loop);
        server->loop = NULL;
        return BUCKETS_ERR_IO;
    }
    
    /* Start listening */
    ret = uv_listen((uv_stream_t*)&server->tcp, server->backlog, on_connection);
    if (ret != 0) {
        buckets_error("Failed to listen: %s", uv_strerror(ret));
        uv_close((uv_handle_t*)&server->tcp, NULL);
        uv_loop_close(server->loop);
        buckets_free(server->loop);
        server->loop = NULL;
        return BUCKETS_ERR_IO;
    }
    
    /* Initialize shutdown async handle */
    ret = uv_async_init(server->loop, &server->shutdown_async, on_shutdown_async);
    if (ret != 0) {
        buckets_error("Failed to init async: %s", uv_strerror(ret));
        uv_close((uv_handle_t*)&server->tcp, NULL);
        uv_loop_close(server->loop);
        buckets_free(server->loop);
        server->loop = NULL;
        return BUCKETS_ERR_IO;
    }
    server->shutdown_async.data = server;
    
    server->running = true;
    
    /* Start server thread */
    ret = pthread_create(&server->thread, NULL, server_thread_main, server);
    if (ret != 0) {
        buckets_error("Failed to create server thread");
        server->running = false;
        uv_close((uv_handle_t*)&server->shutdown_async, NULL);
        uv_close((uv_handle_t*)&server->tcp, NULL);
        uv_loop_close(server->loop);
        buckets_free(server->loop);
        server->loop = NULL;
        return BUCKETS_ERR_IO;
    }
    
    const char *proto = server->tls_enabled ? "https" : "http";
    buckets_info("UV HTTP server started: %s://%s:%d", proto, server->address, server->port);
    
    return BUCKETS_OK;
}

static void* server_thread_main(void *arg)
{
    uv_http_server_t *server = (uv_http_server_t*)arg;
    
    buckets_debug("Server thread started");
    
    /* Run event loop until stopped */
    while (server->running) {
        uv_run(server->loop, UV_RUN_DEFAULT);
    }
    
    buckets_debug("Server thread exiting");
    
    return NULL;
}

static void close_walk_cb(uv_handle_t *handle, void *arg)
{
    (void)arg;
    if (!uv_is_closing(handle)) {
        uv_close(handle, NULL);
    }
}

int uv_http_server_stop(uv_http_server_t *server)
{
    if (!server || !server->running) {
        return BUCKETS_OK;
    }
    
    buckets_info("Stopping UV HTTP server...");
    
    server->running = false;
    
    /* Signal shutdown from event loop */
    uv_async_send(&server->shutdown_async);
    
    /* Wait for thread to finish */
    pthread_join(server->thread, NULL);
    
    /* Close all handles */
    uv_walk(server->loop, close_walk_cb, NULL);
    uv_run(server->loop, UV_RUN_DEFAULT);
    
    /* Clean up loop */
    if (server->owns_loop && server->loop) {
        uv_loop_close(server->loop);
        buckets_free(server->loop);
        server->loop = NULL;
    }
    
    /* Clean up TLS */
    uv_http_tls_cleanup(server);
    
    buckets_info("UV HTTP server stopped");
    
    return BUCKETS_OK;
}

static void on_shutdown_async(uv_async_t *handle)
{
    uv_http_server_t *server = (uv_http_server_t*)handle->data;
    
    /* Close all connections */
    uv_http_conn_t *conn = server->connections;
    while (conn) {
        uv_http_conn_t *next = conn->next;
        uv_http_conn_close(conn);
        conn = next;
    }
    
    /* Stop accepting new connections */
    uv_close((uv_handle_t*)&server->tcp, NULL);
    uv_close((uv_handle_t*)&server->shutdown_async, NULL);
    
    /* Stop the event loop */
    uv_stop(server->loop);
}

void uv_http_server_free(uv_http_server_t *server)
{
    if (!server) return;
    
    /* Stop if running */
    uv_http_server_stop(server);
    
    /* Free routes */
    uv_route_t *route = server->routes;
    while (route) {
        uv_route_t *next = route->next;
        if (route->method) buckets_free(route->method);
        if (route->path_prefix) buckets_free(route->path_prefix);
        buckets_free(route);
        route = next;
    }
    
    pthread_mutex_destroy(&server->lock);
    buckets_free(server);
}

/* ===================================================================
 * Connection Management
 * ===================================================================*/

uv_http_conn_t* uv_http_conn_create(uv_http_server_t *server)
{
    uv_http_conn_t *conn = buckets_calloc(1, sizeof(uv_http_conn_t));
    if (!conn) {
        return NULL;
    }
    
    conn->server = server;
    conn->state = CONN_STATE_READING_HEADERS;
    conn->keep_alive = true;  /* HTTP/1.1 default */
    
    /* Initialize TCP handle */
    int ret = uv_tcp_init(server->loop, &conn->tcp);
    if (ret != 0) {
        buckets_free(conn);
        return NULL;
    }
    conn->tcp.data = conn;
    
    /* Initialize timeout timer */
    ret = uv_timer_init(server->loop, &conn->timeout_timer);
    if (ret != 0) {
        uv_close((uv_handle_t*)&conn->tcp, NULL);
        buckets_free(conn);
        return NULL;
    }
    conn->timeout_timer.data = conn;
    
    /* Initialize HTTP parser */
    setup_parser_callbacks(&conn->parser_settings);
    llhttp_init(&conn->parser, HTTP_REQUEST, &conn->parser_settings);
    conn->parser.data = conn;
    
    /* Initialize TLS if enabled */
    if (server->tls_enabled && server->ssl_ctx) {
        conn->ssl = SSL_new(server->ssl_ctx);
        if (!conn->ssl) {
            uv_close((uv_handle_t*)&conn->timeout_timer, NULL);
            uv_close((uv_handle_t*)&conn->tcp, NULL);
            buckets_free(conn);
            return NULL;
        }
        
        conn->read_bio = BIO_new(BIO_s_mem());
        conn->write_bio = BIO_new(BIO_s_mem());
        SSL_set_bio(conn->ssl, conn->read_bio, conn->write_bio);
        SSL_set_accept_state(conn->ssl);
        
        /* Allocate TLS read buffer */
        conn->tls_read_buffer_capacity = BUCKETS_READ_BUFFER_SIZE;
        conn->tls_read_buffer = buckets_malloc(conn->tls_read_buffer_capacity);
    }
    
    /* Add to server's connection list */
    pthread_mutex_lock(&server->lock);
    conn->next = server->connections;
    if (server->connections) {
        server->connections->prev = conn;
    }
    server->connections = conn;
    server->connection_count++;
    pthread_mutex_unlock(&server->lock);
    
    return conn;
}

void uv_http_conn_reset(uv_http_conn_t *conn)
{
    /* Reinitialize parser completely for next request */
    llhttp_init(&conn->parser, HTTP_REQUEST, &conn->parser_settings);
    conn->parser.data = conn;
    
    /* Free URL */
    if (conn->url) {
        buckets_free(conn->url);
        conn->url = NULL;
        conn->url_len = 0;
        conn->url_capacity = 0;
    }
    
    /* Free headers */
    uv_http_header_t *header = conn->headers;
    while (header) {
        uv_http_header_t *next = header->next;
        buckets_free(header->name);
        buckets_free(header->value);
        buckets_free(header);
        header = next;
    }
    conn->headers = NULL;
    conn->headers_tail = NULL;
    conn->total_headers_size = 0;
    
    /* Free current header parsing state */
    if (conn->current_header_name) {
        buckets_free(conn->current_header_name);
        conn->current_header_name = NULL;
        conn->current_header_name_len = 0;
        conn->current_header_name_capacity = 0;
    }
    if (conn->current_header_value) {
        buckets_free(conn->current_header_value);
        conn->current_header_value = NULL;
        conn->current_header_value_len = 0;
        conn->current_header_value_capacity = 0;
    }
    
    /* Free body */
    if (conn->body) {
        buckets_free(conn->body);
        conn->body = NULL;
        conn->body_len = 0;
        conn->body_capacity = 0;
    }
    conn->content_length = 0;
    
    /* Reset response state */
    conn->response_started = false;
    conn->response_chunked = false;
    conn->message_complete = false;
    if (conn->write_buffer) {
        buckets_free(conn->write_buffer);
        conn->write_buffer = NULL;
        conn->write_buffer_len = 0;
    }
    
    /* Reset connection state */
    conn->state = CONN_STATE_READING_HEADERS;
    conn->requests_served++;
}

void uv_http_conn_close(uv_http_conn_t *conn)
{
    if (!conn || conn->state == CONN_STATE_CLOSING) {
        return;
    }
    
    conn->state = CONN_STATE_CLOSING;
    
    /* Stop timeout timer */
    uv_timer_stop(&conn->timeout_timer);
    
    /* Count how many handles we need to close */
    conn->pending_close_count = 0;
    
    /* Close handles - use the same callback for both so we know when all are done */
    if (!uv_is_closing((uv_handle_t*)&conn->timeout_timer)) {
        conn->pending_close_count++;
        uv_close((uv_handle_t*)&conn->timeout_timer, on_handle_close);
    }
    if (!uv_is_closing((uv_handle_t*)&conn->tcp)) {
        conn->pending_close_count++;
        uv_close((uv_handle_t*)&conn->tcp, on_handle_close);
    }
    
    /* If no handles needed closing (shouldn't happen), free immediately */
    if (conn->pending_close_count == 0) {
        buckets_warn("No handles to close for connection");
    }
}

/**
 * Common close callback for all connection handles.
 * Only frees the connection when ALL handles have finished closing.
 */
static void on_handle_close(uv_handle_t *handle)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)handle->data;
    if (!conn) return;
    
    /* Decrement pending close count */
    conn->pending_close_count--;
    
    /* If there are still handles pending, wait for them */
    if (conn->pending_close_count > 0) {
        return;
    }
    
    /* All handles are now closed - safe to free the connection */
    uv_http_server_t *server = conn->server;
    
    /* Remove from server's connection list */
    pthread_mutex_lock(&server->lock);
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else {
        server->connections = conn->next;
    }
    if (conn->next) {
        conn->next->prev = conn->prev;
    }
    server->connection_count--;
    pthread_mutex_unlock(&server->lock);
    
    /* Clean up TLS */
    if (conn->ssl) {
        SSL_free(conn->ssl);  /* Also frees BIOs */
        conn->ssl = NULL;
    }
    if (conn->tls_read_buffer) {
        buckets_free(conn->tls_read_buffer);
    }
    
    /* Clean up request state */
    uv_http_conn_reset(conn);
    
    buckets_free(conn);
}

/* ===================================================================
 * Timeout Management
 * ===================================================================*/

void uv_http_conn_reset_timeout(uv_http_conn_t *conn, uint64_t timeout_ms)
{
    uv_timer_stop(&conn->timeout_timer);
    uv_timer_start(&conn->timeout_timer, on_timeout, timeout_ms, 0);
}

void uv_http_conn_stop_timeout(uv_http_conn_t *conn)
{
    uv_timer_stop(&conn->timeout_timer);
}

static void on_timeout(uv_timer_t *timer)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)timer->data;
    
    buckets_debug("Connection timeout (state=%d)", conn->state);
    
    /* Send timeout response if we haven't started a response */
    if (!conn->response_started && conn->state != CONN_STATE_CLOSING) {
        const char *response = "HTTP/1.1 408 Request Timeout\r\n"
                              "Connection: close\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n";
        uv_buf_t buf = uv_buf_init((char*)response, strlen(response));
        
        /* Best effort write - don't wait for completion */
        uv_try_write((uv_stream_t*)&conn->tcp, &buf, 1);
    }
    
    uv_http_conn_close(conn);
}

/* ===================================================================
 * Connection Handling
 * ===================================================================*/

static void on_connection(uv_stream_t *server_handle, int status)
{
    uv_http_server_t *server = (uv_http_server_t*)server_handle->data;
    
    if (status < 0) {
        buckets_error("Connection error: %s", uv_strerror(status));
        return;
    }
    
    /* Check connection limit */
    if (server->connection_count >= server->max_connections) {
        buckets_warn("Connection limit reached (%d)", server->max_connections);
        
        /* Accept and immediately close */
        uv_tcp_t temp;
        uv_tcp_init(server->loop, &temp);
        if (uv_accept(server_handle, (uv_stream_t*)&temp) == 0) {
            uv_close((uv_handle_t*)&temp, NULL);
        }
        return;
    }
    
    /* Create new connection */
    uv_http_conn_t *conn = uv_http_conn_create(server);
    if (!conn) {
        buckets_error("Failed to create connection");
        return;
    }
    
    /* Accept connection */
    int ret = uv_accept(server_handle, (uv_stream_t*)&conn->tcp);
    if (ret != 0) {
        buckets_error("Failed to accept: %s", uv_strerror(ret));
        uv_http_conn_close(conn);
        return;
    }
    
    /* Enable TCP_NODELAY for lower latency */
    uv_tcp_nodelay(&conn->tcp, 1);
    
    /* Start timeout for headers */
    uv_http_conn_reset_timeout(conn, server->headers_timeout_ms);
    
    /* Start reading */
    ret = uv_read_start((uv_stream_t*)&conn->tcp, on_alloc, on_read);
    if (ret != 0) {
        buckets_error("Failed to start read: %s", uv_strerror(ret));
        uv_http_conn_close(conn);
        return;
    }
}

static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    (void)suggested_size;
    uv_http_conn_t *conn = (uv_http_conn_t*)handle->data;
    
    /* Use connection's read buffer */
    buf->base = conn->read_buffer;
    buf->len = sizeof(conn->read_buffer);
}

/* ===================================================================
 * TLS Handling
 * ===================================================================*/

static int tls_flush_write_bio(uv_http_conn_t *conn)
{
    char buf[16384];
    int pending;
    
    while ((pending = BIO_pending(conn->write_bio)) > 0) {
        int n = BIO_read(conn->write_bio, buf, sizeof(buf));
        if (n <= 0) break;
        
        /* Write to TCP */
        char *write_buf = buckets_malloc(n);
        memcpy(write_buf, buf, n);
        
        uv_buf_t uv_buf = uv_buf_init(write_buf, n);
        uv_write_t *req = buckets_malloc(sizeof(uv_write_t));
        req->data = write_buf;
        
        int ret = uv_write(req, (uv_stream_t*)&conn->tcp, &uv_buf, 1, on_write_complete);
        if (ret != 0) {
            buckets_free(write_buf);
            buckets_free(req);
            return -1;
        }
    }
    
    return 0;
}

static int tls_do_handshake(uv_http_conn_t *conn)
{
    int ret = SSL_do_handshake(conn->ssl);
    
    if (ret == 1) {
        /* Handshake complete */
        conn->tls_handshake_complete = true;
        buckets_debug("TLS handshake complete");
        return 0;
    }
    
    int err = SSL_get_error(conn->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        /* Need more data or need to write */
        tls_flush_write_bio(conn);
        return 1;  /* Continue */
    }
    
    /* Error */
    buckets_error("TLS handshake failed: %d", err);
    ERR_print_errors_fp(stderr);
    return -1;
}

static int process_tls_data(uv_http_conn_t *conn, const char *data, ssize_t len)
{
    /* Write encrypted data to read BIO */
    BIO_write(conn->read_bio, data, len);
    
    if (!conn->tls_handshake_complete) {
        /* Continue handshake */
        int ret = tls_do_handshake(conn);
        if (ret < 0) {
            return -1;  /* Handshake failed */
        }
        if (ret > 0) {
            return 0;  /* Need more data */
        }
        /* Fall through if handshake just completed */
    }
    
    /* Decrypt data */
    char decrypted[BUCKETS_READ_BUFFER_SIZE];
    int n;
    
    while ((n = SSL_read(conn->ssl, decrypted, sizeof(decrypted))) > 0) {
        /* Process decrypted HTTP data */
        llhttp_errno_t err = llhttp_execute(&conn->parser, decrypted, n);
        
        if (err != HPE_OK && err != HPE_PAUSED) {
            buckets_error("HTTP parse error: %s", llhttp_errno_name(err));
            return -1;
        }
    }
    
    int ssl_err = SSL_get_error(conn->ssl, n);
    if (ssl_err != SSL_ERROR_WANT_READ && ssl_err != SSL_ERROR_ZERO_RETURN) {
        if (ssl_err != SSL_ERROR_NONE) {
            buckets_error("SSL_read error: %d", ssl_err);
            return -1;
        }
    }
    
    return 0;
}

/* ===================================================================
 * Data Reading
 * ===================================================================*/

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)stream->data;
    (void)buf;  /* We use conn->read_buffer */
    
    if (nread < 0) {
        if (nread != UV_EOF) {
            buckets_debug("Read error: %s", uv_strerror(nread));
        }
        uv_http_conn_close(conn);
        return;
    }
    
    if (nread == 0) {
        return;  /* EAGAIN */
    }
    
    /* Reset timeout based on state */
    if (conn->state == CONN_STATE_READING_HEADERS) {
        uv_http_conn_reset_timeout(conn, conn->server->headers_timeout_ms);
    } else if (conn->state == CONN_STATE_READING_BODY) {
        uv_http_conn_reset_timeout(conn, conn->server->body_timeout_ms);
    } else if (conn->state == CONN_STATE_KEEPALIVE_WAIT) {
        uv_http_conn_reset_timeout(conn, conn->server->keepalive_timeout_ms);
        conn->state = CONN_STATE_READING_HEADERS;
    }
    
    /* Handle TLS or plain text */
    if (conn->ssl) {
        if (process_tls_data(conn, conn->read_buffer, nread) < 0) {
            uv_http_conn_close(conn);
        }
    } else {
        /* Plain HTTP - parse directly */
        llhttp_errno_t err = llhttp_execute(&conn->parser, conn->read_buffer, nread);
        
        if (err != HPE_OK && err != HPE_PAUSED) {
            buckets_error("HTTP parse error: %s (%s) [state=%d, requests=%d]", 
                         llhttp_errno_name(err),
                         llhttp_get_error_reason(&conn->parser),
                         conn->state,
                         conn->requests_served);
            
            /* Send error response */
            if (!conn->response_started) {
                const char *response = "HTTP/1.1 400 Bad Request\r\n"
                                      "Connection: close\r\n"
                                      "Content-Length: 0\r\n"
                                      "\r\n";
                uv_buf_t resp_buf = uv_buf_init((char*)response, strlen(response));
                uv_try_write((uv_stream_t*)&conn->tcp, &resp_buf, 1);
            }
            
            uv_http_conn_close(conn);
        }
        
        /* Process request AFTER llhttp_execute() returns - this allows us
         * to safely reinitialize the parser for keep-alive connections. */
        if (conn->message_complete) {
            conn->message_complete = false;
            process_request(conn);
        }
    }
}

/* ===================================================================
 * HTTP Parser Callbacks
 * ===================================================================*/

static void setup_parser_callbacks(llhttp_settings_t *settings)
{
    llhttp_settings_init(settings);
    
    settings->on_message_begin = on_message_begin;
    settings->on_url = on_url;
    settings->on_url_complete = on_url_complete;
    settings->on_header_field = on_header_field;
    settings->on_header_field_complete = on_header_field_complete;
    settings->on_header_value = on_header_value;
    settings->on_header_value_complete = on_header_value_complete;
    settings->on_headers_complete = on_headers_complete;
    settings->on_body = on_body;
    settings->on_message_complete = on_message_complete;
}

int on_message_begin(llhttp_t *parser)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)parser->data;
    
    /* Reset for new request (already done if keep-alive) */
    conn->state = CONN_STATE_READING_HEADERS;
    
    return 0;
}

int on_url(llhttp_t *parser, const char *at, size_t len)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)parser->data;
    
    /* Grow URL buffer if needed */
    size_t needed = conn->url_len + len + 1;
    if (needed > conn->url_capacity) {
        size_t new_cap = conn->url_capacity ? conn->url_capacity * 2 : 256;
        while (new_cap < needed) new_cap *= 2;
        
        char *new_url = buckets_realloc(conn->url, new_cap);
        if (!new_url) return -1;
        
        conn->url = new_url;
        conn->url_capacity = new_cap;
    }
    
    memcpy(conn->url + conn->url_len, at, len);
    conn->url_len += len;
    conn->url[conn->url_len] = '\0';
    
    return 0;
}

int on_url_complete(llhttp_t *parser)
{
    (void)parser;
    return 0;
}

int on_header_field(llhttp_t *parser, const char *at, size_t len)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)parser->data;
    
    /* Check headers size limit */
    conn->total_headers_size += len;
    if (conn->total_headers_size > BUCKETS_MAX_HEADERS_SIZE) {
        return -1;
    }
    
    /* Grow buffer if needed */
    size_t needed = conn->current_header_name_len + len + 1;
    if (needed > conn->current_header_name_capacity) {
        size_t new_cap = conn->current_header_name_capacity ? 
                         conn->current_header_name_capacity * 2 : 64;
        while (new_cap < needed) new_cap *= 2;
        
        char *new_buf = buckets_realloc(conn->current_header_name, new_cap);
        if (!new_buf) return -1;
        
        conn->current_header_name = new_buf;
        conn->current_header_name_capacity = new_cap;
    }
    
    memcpy(conn->current_header_name + conn->current_header_name_len, at, len);
    conn->current_header_name_len += len;
    conn->current_header_name[conn->current_header_name_len] = '\0';
    
    return 0;
}

int on_header_field_complete(llhttp_t *parser)
{
    (void)parser;
    return 0;
}

int on_header_value(llhttp_t *parser, const char *at, size_t len)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)parser->data;
    
    conn->total_headers_size += len;
    if (conn->total_headers_size > BUCKETS_MAX_HEADERS_SIZE) {
        return -1;
    }
    
    /* Grow buffer if needed */
    size_t needed = conn->current_header_value_len + len + 1;
    if (needed > conn->current_header_value_capacity) {
        size_t new_cap = conn->current_header_value_capacity ?
                         conn->current_header_value_capacity * 2 : 256;
        while (new_cap < needed) new_cap *= 2;
        
        char *new_buf = buckets_realloc(conn->current_header_value, new_cap);
        if (!new_buf) return -1;
        
        conn->current_header_value = new_buf;
        conn->current_header_value_capacity = new_cap;
    }
    
    memcpy(conn->current_header_value + conn->current_header_value_len, at, len);
    conn->current_header_value_len += len;
    conn->current_header_value[conn->current_header_value_len] = '\0';
    
    return 0;
}

int on_header_value_complete(llhttp_t *parser)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)parser->data;
    
    /* Create header entry */
    uv_http_header_t *header = buckets_calloc(1, sizeof(uv_http_header_t));
    if (!header) return -1;
    
    header->name = conn->current_header_name;
    header->name_len = conn->current_header_name_len;
    header->value = conn->current_header_value;
    header->value_len = conn->current_header_value_len;
    
    /* Reset current header buffers (ownership transferred) */
    conn->current_header_name = NULL;
    conn->current_header_name_len = 0;
    conn->current_header_name_capacity = 0;
    conn->current_header_value = NULL;
    conn->current_header_value_len = 0;
    conn->current_header_value_capacity = 0;
    
    /* Add to list */
    if (conn->headers_tail) {
        conn->headers_tail->next = header;
    } else {
        conn->headers = header;
    }
    conn->headers_tail = header;
    
    return 0;
}

int on_headers_complete(llhttp_t *parser)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)parser->data;
    
    /* Get content length */
    conn->content_length = parser->content_length;
    
    /* Check Connection header for keep-alive */
    const char *connection = uv_http_get_header(conn, "Connection");
    if (connection) {
        if (strcasecmp(connection, "close") == 0) {
            conn->keep_alive = false;
        } else if (strcasecmp(connection, "keep-alive") == 0) {
            conn->keep_alive = true;
        }
    } else {
        /* HTTP/1.1 defaults to keep-alive */
        conn->keep_alive = (parser->http_major == 1 && parser->http_minor == 1);
    }
    
    conn->state = CONN_STATE_READING_BODY;
    
    /* Reset timeout for body */
    uv_http_conn_reset_timeout(conn, conn->server->body_timeout_ms);
    
    /* Check for streaming handler */
    uv_route_t *route = find_streaming_route(conn);
    if (route) {
        conn->streaming_route = route;
        
        /* Build stream request and call on_request_start */
        uv_stream_request_t stream_req;
        build_stream_request(conn, &stream_req);
        
        int ret = route->handler.streaming.on_request_start(&stream_req, 
                                                             route->handler.streaming.user_data);
        if (ret != 0) {
            /* Handler rejected the request - fall back to buffered mode */
            conn->streaming_route = NULL;
        }
    }
    
    return 0;
}

int on_body(llhttp_t *parser, const char *at, size_t len)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)parser->data;
    
    /* For streaming handlers, call on_body_chunk */
    if (conn->streaming_route) {
        uv_route_t *route = conn->streaming_route;
        
        uv_stream_request_t stream_req;
        build_stream_request(conn, &stream_req);
        
        int ret = route->handler.streaming.on_body_chunk(&stream_req, at, len,
                                                          route->handler.streaming.user_data);
        if (ret != 0) {
            /* Handler error - abort streaming */
            if (route->handler.streaming.on_request_error) {
                route->handler.streaming.on_request_error(&stream_req, ret,
                                                           route->handler.streaming.user_data);
            }
            conn->streaming_route = NULL;
            return -1;
        }
        return 0;
    }
    
    /* For non-streaming: buffer the body */
    size_t needed = conn->body_len + len;
    if (needed > conn->body_capacity) {
        size_t new_cap = conn->body_capacity ? conn->body_capacity : BUCKETS_INITIAL_BODY_BUFFER;
        while (new_cap < needed) new_cap *= 2;
        
        char *new_body = buckets_realloc(conn->body, new_cap);
        if (!new_body) {
            buckets_error("Failed to allocate body buffer (%zu bytes)", new_cap);
            return -1;
        }
        
        conn->body = new_body;
        conn->body_capacity = new_cap;
    }
    
    memcpy(conn->body + conn->body_len, at, len);
    conn->body_len += len;
    
    return 0;
}

int on_message_complete(llhttp_t *parser)
{
    uv_http_conn_t *conn = (uv_http_conn_t*)parser->data;
    
    /* Stop timeout during processing */
    uv_http_conn_stop_timeout(conn);
    
    conn->state = CONN_STATE_PROCESSING;
    
    /* For streaming handlers, call on_request_complete */
    if (conn->streaming_route) {
        uv_route_t *route = conn->streaming_route;
        
        uv_stream_request_t stream_req;
        build_stream_request(conn, &stream_req);
        
        route->handler.streaming.on_request_complete(&stream_req,
                                                      route->handler.streaming.user_data);
        
        /* Clear streaming route */
        conn->streaming_route = NULL;
        
        /* Mark message complete so normal flow handles keep-alive after response
         * is fully written. process_request will see response_started=true and
         * skip sending another response. */
        conn->message_complete = true;
        return 0;
    }
    
    /* Mark that we need to process - don't call process_request here
     * because we're still inside llhttp_execute() and calling llhttp_init()
     * during parsing causes issues with keep-alive. */
    conn->message_complete = true;
    
    return 0;
}

/* ===================================================================
 * Header Access
 * ===================================================================*/

const char* uv_http_get_header(uv_http_conn_t *conn, const char *name)
{
    uv_http_header_t *header = conn->headers;
    while (header) {
        if (strcasecmp(header->name, name) == 0) {
            return header->value;
        }
        header = header->next;
    }
    return NULL;
}

/* ===================================================================
 * Streaming Route Matching
 * ===================================================================*/

/**
 * Find a streaming route that matches the request
 */
static uv_route_t* find_streaming_route(uv_http_conn_t *conn)
{
    uv_http_server_t *server = conn->server;
    
    /* Parse path from URL */
    char *query = strchr(conn->url, '?');
    size_t path_len = query ? (size_t)(query - conn->url) : conn->url_len;
    
    /* Get method */
    const char *method = llhttp_method_name(llhttp_get_method(&conn->parser));
    
    /* Search for matching streaming route */
    uv_route_t *route = server->routes;
    while (route) {
        if (!route->is_streaming) {
            route = route->next;
            continue;
        }
        
        /* Check method if specified */
        if (route->method && strcasecmp(route->method, method) != 0) {
            route = route->next;
            continue;
        }
        
        /* Check path prefix */
        size_t prefix_len = strlen(route->path_prefix);
        if (path_len >= prefix_len && 
            strncmp(conn->url, route->path_prefix, prefix_len) == 0) {
            return route;
        }
        
        route = route->next;
    }
    
    return NULL;
}

/**
 * Build streaming request from connection state
 */
static void build_stream_request(uv_http_conn_t *conn, uv_stream_request_t *req)
{
    req->conn = conn;
    req->method = llhttp_get_method(&conn->parser);
    req->url = conn->url;
    req->url_len = conn->url_len;
    req->query_string = strchr(conn->url, '?');
    req->content_length = conn->content_length;
    req->chunked_encoding = false;  /* TODO: detect from headers */
    req->headers = conn->headers;
}

/* ===================================================================
 * Request Processing
 * ===================================================================*/

static void process_request(uv_http_conn_t *conn)
{
    uv_http_server_t *server = conn->server;
    
    /* If response already started (e.g., from streaming handler), skip to done */
    if (conn->response_started) {
        goto done;
    }
    
    /* Parse query string from URL */
    char *query = strchr(conn->url, '?');
    char *path = conn->url;
    size_t path_len = query ? (size_t)(query - conn->url) : conn->url_len;
    
    /* Get method string */
    const char *method = llhttp_method_name(llhttp_get_method(&conn->parser));
    
    buckets_debug("Request: %s %.*s", method, (int)path_len, path);
    
    /* Find matching route */
    uv_route_t *route = server->routes;
    while (route) {
        /* Check method if specified */
        if (route->method && strcasecmp(route->method, method) != 0) {
            route = route->next;
            continue;
        }
        
        /* Skip streaming routes - they're handled in on_headers_complete */
        if (route->is_streaming) {
            route = route->next;
            continue;
        }
        
        /* Check path prefix */
        size_t prefix_len = strlen(route->path_prefix);
        if (path_len >= prefix_len && 
            strncmp(path, route->path_prefix, prefix_len) == 0) {
            /* Match found */
            route->handler.legacy(conn, route->user_data);
            goto done;
        }
        
        route = route->next;
    }
    
    /* Use default handler */
    if (server->default_handler) {
        server->default_handler(conn, server->default_handler_data);
    } else {
        /* No handler - 404 */
        uv_http_response_start(conn, 404, NULL, 0, 0);
        uv_http_response_end(conn);
    }
    
done:
    /* Handle keep-alive or close */
    if (!conn->response_started) {
        /* Handler didn't send response - send 500 */
        uv_http_response_start(conn, 500, NULL, 0, 0);
        uv_http_response_end(conn);
    }
    
    if (conn->keep_alive && conn->state != CONN_STATE_CLOSING) {
        /* Reset for next request */
        uv_http_conn_reset(conn);
        conn->state = CONN_STATE_KEEPALIVE_WAIT;
        uv_http_conn_reset_timeout(conn, server->keepalive_timeout_ms);
    } else {
        uv_http_conn_close(conn);
    }
}

/* ===================================================================
 * Response Writing
 * ===================================================================*/

int uv_http_response_start(uv_http_conn_t *conn, int status,
                            const char *headers[], int num_headers,
                            size_t content_length)
{
    if (conn->response_started) {
        buckets_error("Response already started");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Build response header */
    char header_buf[4096];
    int offset = snprintf(header_buf, sizeof(header_buf),
                         "HTTP/1.1 %d %s\r\n",
                         status, http_status_string(status));
    
    /* Add custom headers */
    for (int i = 0; i < num_headers && headers && headers[i]; i += 2) {
        offset += snprintf(header_buf + offset, sizeof(header_buf) - offset,
                          "%s: %s\r\n", headers[i], headers[i+1]);
    }
    
    /* Add Content-Length header
     * Note: We use Content-Length for all responses with known size (including 0).
     * This avoids chunked encoding issues with empty responses.
     * Chunked encoding would be used for streaming responses where size is unknown.
     */
    offset += snprintf(header_buf + offset, sizeof(header_buf) - offset,
                      "Content-Length: %zu\r\n", content_length);
    conn->response_chunked = false;
    
    /* Add Connection header */
    offset += snprintf(header_buf + offset, sizeof(header_buf) - offset,
                      "Connection: %s\r\n",
                      conn->keep_alive ? "keep-alive" : "close");
    
    /* End headers */
    offset += snprintf(header_buf + offset, sizeof(header_buf) - offset, "\r\n");
    
    /* Write headers directly (not chunked) */
    char *write_buf = buckets_malloc(offset);
    if (!write_buf) return BUCKETS_ERR_NOMEM;
    memcpy(write_buf, header_buf, offset);
    
    uv_buf_t buf = uv_buf_init(write_buf, offset);
    uv_write_t *req = buckets_malloc(sizeof(uv_write_t));
    if (!req) {
        buckets_free(write_buf);
        return BUCKETS_ERR_NOMEM;
    }
    req->data = write_buf;
    
    conn->response_started = true;
    
    return uv_write(req, (uv_stream_t*)&conn->tcp, &buf, 1, on_write_complete);
}

int uv_http_response_write(uv_http_conn_t *conn, const void *data, size_t len)
{
    if (len == 0) return BUCKETS_OK;
    
    /* Allocate write buffer */
    char *write_buf;
    size_t write_len;
    
    if (conn->response_chunked && conn->response_started) {
        /* Chunked encoding: add chunk header */
        write_len = len + 32;  /* Space for chunk header and trailer */
        write_buf = buckets_malloc(write_len);
        if (!write_buf) return BUCKETS_ERR_NOMEM;
        
        int header_len = snprintf(write_buf, 32, "%zx\r\n", len);
        memcpy(write_buf + header_len, data, len);
        memcpy(write_buf + header_len + len, "\r\n", 2);
        write_len = header_len + len + 2;
    } else {
        /* Direct write */
        write_buf = buckets_malloc(len);
        if (!write_buf) return BUCKETS_ERR_NOMEM;
        memcpy(write_buf, data, len);
        write_len = len;
    }
    
    /* Handle TLS */
    if (conn->ssl && conn->tls_handshake_complete) {
        int n = SSL_write(conn->ssl, write_buf, write_len);
        buckets_free(write_buf);
        
        if (n <= 0) {
            int err = SSL_get_error(conn->ssl, n);
            if (err != SSL_ERROR_WANT_WRITE) {
                return BUCKETS_ERR_IO;
            }
        }
        
        /* Flush write BIO to TCP */
        return tls_flush_write_bio(conn);
    }
    
    /* Plain TCP write */
    uv_buf_t buf = uv_buf_init(write_buf, write_len);
    uv_write_t *req = buckets_malloc(sizeof(uv_write_t));
    if (!req) {
        buckets_free(write_buf);
        return BUCKETS_ERR_NOMEM;
    }
    req->data = write_buf;
    
    int ret = uv_write(req, (uv_stream_t*)&conn->tcp, &buf, 1, on_write_complete);
    if (ret != 0) {
        buckets_free(write_buf);
        buckets_free(req);
        return BUCKETS_ERR_IO;
    }
    
    return BUCKETS_OK;
}

int uv_http_response_end(uv_http_conn_t *conn)
{
    if (conn->response_chunked && conn->response_started) {
        /* Send terminating chunk for chunked encoding: "0\r\n\r\n" */
        const char *terminator = "0\r\n\r\n";
        size_t terminator_len = 5;
        
        char *write_buf = buckets_malloc(terminator_len);
        if (!write_buf) return BUCKETS_ERR_NOMEM;
        memcpy(write_buf, terminator, terminator_len);
        
        /* For TLS, the write goes through SSL layer in uv_http_response_write
         * But since that checks for len==0, we need to write directly here. */
        uv_buf_t buf = uv_buf_init(write_buf, terminator_len);
        uv_write_t *req = buckets_malloc(sizeof(uv_write_t));
        if (!req) {
            buckets_free(write_buf);
            return BUCKETS_ERR_NOMEM;
        }
        req->data = write_buf;
        
        return uv_write(req, (uv_stream_t*)&conn->tcp, &buf, 1, on_write_complete);
    }
    return BUCKETS_OK;
}

static void on_write_complete(uv_write_t *req, int status)
{
    char *buf = (char*)req->data;
    if (buf) buckets_free(buf);
    buckets_free(req);
    
    if (status < 0) {
        /* Get connection from write request */
        uv_http_conn_t *conn = (uv_http_conn_t*)req->handle->data;
        if (conn) {
            buckets_debug("Write failed: %s", uv_strerror(status));
            uv_http_conn_close(conn);
        }
    }
}

/* ===================================================================
 * Public API Wrappers (for compatibility with existing code)
 * ===================================================================*/

/* These functions provide compatibility with the existing buckets_http_* API */

buckets_http_server_t* buckets_uv_http_server_create(const char *addr, int port)
{
    return (buckets_http_server_t*)uv_http_server_create(addr, port);
}

int buckets_uv_http_server_start(buckets_http_server_t *server)
{
    return uv_http_server_start((uv_http_server_t*)server);
}

int buckets_uv_http_server_stop(buckets_http_server_t *server)
{
    return uv_http_server_stop((uv_http_server_t*)server);
}

void buckets_uv_http_server_free(buckets_http_server_t *server)
{
    uv_http_server_free((uv_http_server_t*)server);
}
