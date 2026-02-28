/**
 * UV Server Internal Structures
 * 
 * Internal definitions for the libuv-based HTTP server.
 * Not exposed in public API.
 */

#ifndef UV_SERVER_INTERNAL_H
#define UV_SERVER_INTERNAL_H

#include <uv.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "../../third_party/llhttp/include/llhttp.h"

/* ===================================================================
 * Configuration Constants
 * ===================================================================*/

/* Connection limits */
#define BUCKETS_DEFAULT_MAX_CONNECTIONS      10000
#define BUCKETS_DEFAULT_BACKLOG              128

/* Timeouts (milliseconds) */
#define BUCKETS_DEFAULT_HEADERS_TIMEOUT_MS   30000   /* 30 seconds */
#define BUCKETS_DEFAULT_BODY_TIMEOUT_MS      300000  /* 5 minutes between chunks */
#define BUCKETS_DEFAULT_KEEPALIVE_TIMEOUT_MS 120000  /* 2 minutes */
#define BUCKETS_DEFAULT_WRITE_TIMEOUT_MS     300000  /* 5 minutes */

/* Buffer sizes */
#define BUCKETS_READ_BUFFER_SIZE             65536   /* 64KB read buffer */
#define BUCKETS_MAX_HEADERS_SIZE             65536   /* 64KB max headers */
#define BUCKETS_INITIAL_BODY_BUFFER          262144  /* 256KB initial body buffer */

/* ===================================================================
 * Forward Declarations
 * ===================================================================*/

typedef struct uv_http_server uv_http_server_t;
typedef struct uv_http_conn uv_http_conn_t;
typedef struct uv_http_header uv_http_header_t;
typedef struct uv_route uv_route_t;

/* ===================================================================
 * HTTP Header
 * ===================================================================*/

struct uv_http_header {
    char *name;
    size_t name_len;
    char *value;
    size_t value_len;
    uv_http_header_t *next;
};

/* ===================================================================
 * Connection States
 * ===================================================================*/

typedef enum {
    CONN_STATE_READING_HEADERS,
    CONN_STATE_READING_BODY,
    CONN_STATE_PROCESSING,
    CONN_STATE_WRITING_RESPONSE,
    CONN_STATE_KEEPALIVE_WAIT,
    CONN_STATE_CLOSING
} uv_conn_state_t;

/* ===================================================================
 * HTTP Connection
 * ===================================================================*/

struct uv_http_conn {
    /* libuv handles - tcp MUST be first for casting */
    uv_tcp_t tcp;
    uv_timer_t timeout_timer;
    
    /* Back-pointer to server */
    uv_http_server_t *server;
    
    /* Connection state */
    uv_conn_state_t state;
    bool keep_alive;
    int requests_served;           /* Number of requests on this connection */
    
    /* TLS state */
    SSL *ssl;
    BIO *read_bio;
    BIO *write_bio;
    bool tls_handshake_complete;
    
    /* HTTP parser */
    llhttp_t parser;
    llhttp_settings_t parser_settings;
    
    /* Current request parsing state */
    char *url;
    size_t url_len;
    size_t url_capacity;
    
    /* Headers */
    uv_http_header_t *headers;
    uv_http_header_t *headers_tail;
    char *current_header_name;
    size_t current_header_name_len;
    size_t current_header_name_capacity;
    char *current_header_value;
    size_t current_header_value_len;
    size_t current_header_value_capacity;
    size_t total_headers_size;
    
    /* Body buffer (for non-streaming mode) */
    char *body;
    size_t body_len;
    size_t body_capacity;
    size_t content_length;         /* Expected content length from header */
    
    /* Response state */
    bool response_started;
    bool response_chunked;
    bool message_complete;         /* Set by on_message_complete, processed after parse */
    uv_write_t write_req;
    char *write_buffer;
    size_t write_buffer_len;
    
    /* Streaming handler context */
    void *stream_ctx;
    uv_route_t *streaming_route;   /* Active streaming route, or NULL */
    
    /* Read buffer */
    char read_buffer[BUCKETS_READ_BUFFER_SIZE];
    size_t read_buffer_len;
    
    /* TLS buffers */
    char *tls_read_buffer;
    size_t tls_read_buffer_len;
    size_t tls_read_buffer_capacity;
    
    /* Connection tracking */
    uv_http_conn_t *prev;
    uv_http_conn_t *next;
    
    /* Close synchronization - count of handles pending close */
    int pending_close_count;
};

/* ===================================================================
 * Streaming Handler Types
 * ===================================================================*/

/* Forward declaration for request handle */
typedef struct uv_stream_request uv_stream_request_t;

struct uv_stream_request {
    uv_http_conn_t *conn;
    
    /* Parsed request info (valid after headers complete) */
    llhttp_method_t method;
    char *url;
    size_t url_len;
    char *query_string;            /* Points into url or NULL */
    size_t content_length;
    bool chunked_encoding;
    
    /* Header access */
    uv_http_header_t *headers;
};

/* Streaming handler callbacks */
typedef struct {
    /**
     * Called when request headers are complete.
     * Return 0 to continue, non-zero to abort.
     */
    int (*on_request_start)(uv_stream_request_t *req, void *user_data);
    
    /**
     * Called for each chunk of body data.
     * May be called multiple times. Data pointer is only valid during callback.
     * Return 0 to continue, non-zero to abort.
     */
    int (*on_body_chunk)(uv_stream_request_t *req,
                         const void *data, size_t len,
                         void *user_data);
    
    /**
     * Called when request is complete (all body received).
     * Return 0 on success.
     */
    int (*on_request_complete)(uv_stream_request_t *req, void *user_data);
    
    /**
     * Called on error or connection close.
     */
    void (*on_request_error)(uv_stream_request_t *req, int error, void *user_data);
    
    /* User data passed to all callbacks */
    void *user_data;
} uv_stream_handler_t;

/* Legacy (non-streaming) handler - buffers entire body */
typedef void (*uv_http_handler_t)(uv_http_conn_t *conn, void *user_data);

/* ===================================================================
 * Route Entry
 * ===================================================================*/

struct uv_route {
    char *method;                  /* GET, PUT, etc. or NULL for any */
    char *path_prefix;             /* Path prefix to match */
    bool is_streaming;             /* Use streaming handler? */
    union {
        uv_http_handler_t legacy;
        uv_stream_handler_t streaming;
    } handler;
    void *user_data;
    struct uv_route *next;
};

/* ===================================================================
 * HTTP Server
 * ===================================================================*/

struct uv_http_server {
    /* libuv handles */
    uv_loop_t *loop;
    uv_tcp_t tcp;
    uv_async_t shutdown_async;
    
    /* Server configuration */
    char address[256];
    int port;
    int backlog;
    
    /* Timeouts */
    uint64_t headers_timeout_ms;
    uint64_t body_timeout_ms;
    uint64_t keepalive_timeout_ms;
    uint64_t write_timeout_ms;
    
    /* TLS configuration */
    bool tls_enabled;
    SSL_CTX *ssl_ctx;
    char cert_file[512];
    char key_file[512];
    
    /* Routing */
    uv_route_t *routes;
    uv_http_handler_t default_handler;
    void *default_handler_data;
    
    /* Connection tracking */
    uv_http_conn_t *connections;
    int connection_count;
    int max_connections;
    
    /* Server state */
    pthread_t thread;
    bool running;
    bool owns_loop;                /* Did we create the loop? */
    pthread_mutex_t lock;
};

/* ===================================================================
 * Server API
 * ===================================================================*/

uv_http_server_t* uv_http_server_create(const char *addr, int port);
int uv_http_server_enable_tls(uv_http_server_t *server,
                               const char *cert_file,
                               const char *key_file);
int uv_http_server_set_handler(uv_http_server_t *server,
                                uv_http_handler_t handler,
                                void *user_data);
int uv_http_server_add_route(uv_http_server_t *server,
                              const char *method,
                              const char *path_prefix,
                              uv_http_handler_t handler,
                              void *user_data);
int uv_http_server_add_streaming_route(uv_http_server_t *server,
                                        const char *method,
                                        const char *path_prefix,
                                        uv_stream_handler_t *handler);
int uv_http_server_start(uv_http_server_t *server);
int uv_http_server_stop(uv_http_server_t *server);
void uv_http_server_free(uv_http_server_t *server);

/* ===================================================================
 * Internal Functions
 * ===================================================================*/

/* Connection management */
uv_http_conn_t* uv_http_conn_create(uv_http_server_t *server);
void uv_http_conn_free(uv_http_conn_t *conn);
void uv_http_conn_reset(uv_http_conn_t *conn);  /* Reset for keep-alive */
void uv_http_conn_close(uv_http_conn_t *conn);

/* TLS */
int uv_http_tls_init(uv_http_server_t *server);
void uv_http_tls_cleanup(uv_http_server_t *server);
int uv_http_conn_tls_handshake(uv_http_conn_t *conn);
int uv_http_conn_tls_read(uv_http_conn_t *conn, const char *data, size_t len);
int uv_http_conn_tls_write(uv_http_conn_t *conn, const char *data, size_t len);

/* HTTP parsing callbacks */
int on_message_begin(llhttp_t *parser);
int on_url(llhttp_t *parser, const char *at, size_t len);
int on_url_complete(llhttp_t *parser);
int on_header_field(llhttp_t *parser, const char *at, size_t len);
int on_header_field_complete(llhttp_t *parser);
int on_header_value(llhttp_t *parser, const char *at, size_t len);
int on_header_value_complete(llhttp_t *parser);
int on_headers_complete(llhttp_t *parser);
int on_body(llhttp_t *parser, const char *at, size_t len);
int on_message_complete(llhttp_t *parser);

/* Response helpers */
int uv_http_response_start(uv_http_conn_t *conn, int status,
                           const char *headers[], int num_headers,
                           size_t content_length);
int uv_http_response_write(uv_http_conn_t *conn, const void *data, size_t len);
int uv_http_response_end(uv_http_conn_t *conn);

/* Header access */
const char* uv_http_get_header(uv_http_conn_t *conn, const char *name);

/* Timeout management */
void uv_http_conn_reset_timeout(uv_http_conn_t *conn, uint64_t timeout_ms);
void uv_http_conn_stop_timeout(uv_http_conn_t *conn);

#endif /* UV_SERVER_INTERNAL_H */
