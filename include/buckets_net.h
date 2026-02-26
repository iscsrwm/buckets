/**
 * Network Layer API
 * 
 * Provides HTTP/S server and peer communication for Buckets.
 * 
 * Phase 8: Network Layer (Weeks 31-34)
 */

#ifndef BUCKETS_NET_H
#define BUCKETS_NET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "buckets.h"

/* ===================================================================
 * HTTP Server (Week 31)
 * ===================================================================*/

/**
 * HTTP request structure
 */
typedef struct {
    const char *method;        /* GET, PUT, DELETE, etc. */
    const char *uri;           /* /bucket/object */
    const char *query_string;  /* key=value&... */
    const char *body;          /* Request body */
    size_t body_len;           /* Body length */
    void *internal;            /* Internal mongoose data */
} buckets_http_request_t;

/**
 * HTTP response structure
 */
typedef struct {
    int status_code;           /* 200, 404, 500, etc. */
    char *body;                /* Response body */
    size_t body_len;           /* Body length */
    char *headers;             /* Additional headers */
} buckets_http_response_t;

/**
 * HTTP handler function type
 */
typedef void (*buckets_http_handler_t)(buckets_http_request_t *req,
                                        buckets_http_response_t *res,
                                        void *user_data);

/**
 * HTTP server (opaque)
 */
typedef struct buckets_http_server buckets_http_server_t;

/**
 * Create HTTP server
 * 
 * @param addr Address to bind (e.g., "0.0.0.0" or "127.0.0.1")
 * @param port Port to listen on (e.g., 9000)
 * @return Server handle or NULL on error
 */
buckets_http_server_t* buckets_http_server_create(const char *addr, int port);

/**
 * Set default handler for unmatched routes
 * 
 * @param server Server handle
 * @param handler Default handler function
 * @param user_data User data passed to handler
 * @return BUCKETS_OK on success
 */
int buckets_http_server_set_default_handler(buckets_http_server_t *server,
                                              buckets_http_handler_t handler,
                                              void *user_data);

/**
 * Start HTTP server
 * 
 * Starts the server in a background thread.
 * 
 * @param server Server handle
 * @return BUCKETS_OK on success
 */
int buckets_http_server_start(buckets_http_server_t *server);

/**
 * Stop HTTP server
 * 
 * @param server Server handle
 * @return BUCKETS_OK on success
 */
int buckets_http_server_stop(buckets_http_server_t *server);

/**
 * Free HTTP server
 * 
 * @param server Server handle
 */
void buckets_http_server_free(buckets_http_server_t *server);

/**
 * Get server listening address
 * 
 * @param server Server handle
 * @param addr Buffer to write address (e.g., "http://127.0.0.1:9000")
 * @param len Buffer length
 * @return BUCKETS_OK on success
 */
int buckets_http_server_get_address(buckets_http_server_t *server,
                                      char *addr, size_t len);

/* ===================================================================
 * Router (Week 31)
 * ===================================================================*/

/**
 * HTTP router (opaque)
 */
typedef struct buckets_router buckets_router_t;

/**
 * Route match result
 */
typedef struct {
    buckets_http_handler_t handler;  /* Matched handler */
    void *user_data;                  /* User data for handler */
    bool matched;                     /* true if route was matched */
} buckets_route_match_t;

/**
 * Create router
 * 
 * @return Router handle or NULL on error
 */
buckets_router_t* buckets_router_create(void);

/**
 * Add route to router
 * 
 * @param router Router handle
 * @param method HTTP method (e.g., "GET", "PUT", "*" for all)
 * @param path URL path pattern (e.g., "/buckets/wildcard", "/health")
 * @param handler Handler function for this route
 * @param user_data User data passed to handler
 * @return BUCKETS_OK on success
 */
int buckets_router_add_route(buckets_router_t *router,
                              const char *method,
                              const char *path,
                              buckets_http_handler_t handler,
                              void *user_data);

/**
 * Match request to route
 * 
 * @param router Router handle
 * @param method HTTP method
 * @param path URL path
 * @param match Output: match result
 * @return BUCKETS_OK on success
 */
int buckets_router_match(buckets_router_t *router,
                          const char *method,
                          const char *path,
                          buckets_route_match_t *match);

/**
 * Get number of routes in router
 * 
 * @param router Router handle
 * @return Number of routes
 */
int buckets_router_get_route_count(buckets_router_t *router);

/**
 * Free router
 * 
 * @param router Router handle
 */
void buckets_router_free(buckets_router_t *router);

/* ===================================================================
 * HTTP Response Helpers
 * ===================================================================*/

/**
 * Set response status and body
 * 
 * @param res Response structure
 * @param status_code HTTP status code
 * @param body Response body
 * @param body_len Body length
 */
void buckets_http_response_set(buckets_http_response_t *res,
                                 int status_code,
                                 const char *body,
                                 size_t body_len);

/**
 * Set response header
 * 
 * @param res Response structure
 * @param name Header name
 * @param value Header value
 */
void buckets_http_response_set_header(buckets_http_response_t *res,
                                        const char *name,
                                        const char *value);

/**
 * Send JSON response
 * 
 * @param res Response structure
 * @param status_code HTTP status code
 * @param json JSON string
 */
void buckets_http_response_json(buckets_http_response_t *res,
                                  int status_code,
                                  const char *json);

/**
 * Send error response
 * 
 * @param res Response structure
 * @param status_code HTTP status code
 * @param message Error message
 */
void buckets_http_response_error(buckets_http_response_t *res,
                                   int status_code,
                                   const char *message);

/* ===================================================================
 * TLS Support (Week 32)
 * ===================================================================*/

/**
 * TLS configuration
 */
typedef struct {
    const char *cert_file;     /* Server certificate file */
    const char *key_file;      /* Private key file */
    const char *ca_file;       /* CA bundle file (optional) */
} buckets_tls_config_t;

/**
 * Enable TLS/HTTPS on server
 * 
 * @param server Server handle
 * @param config TLS configuration
 * @return BUCKETS_OK on success
 */
int buckets_http_server_enable_tls(buckets_http_server_t *server,
                                     buckets_tls_config_t *config);

/* ===================================================================
 * Connection Pool (Week 32)
 * ===================================================================*/

/**
 * Connection handle (opaque)
 */
typedef struct buckets_connection buckets_connection_t;

/**
 * Connection pool (opaque)
 */
typedef struct buckets_conn_pool buckets_conn_pool_t;

/**
 * Create connection pool
 * 
 * @param max_conns Maximum number of connections (0 = unlimited)
 * @return Connection pool handle or NULL on error
 */
buckets_conn_pool_t* buckets_conn_pool_create(int max_conns);

/**
 * Get connection from pool
 * 
 * Creates new connection if none available, or reuses existing.
 * 
 * @param pool Connection pool
 * @param host Target host
 * @param port Target port
 * @param conn Output: connection handle
 * @return BUCKETS_OK on success
 */
int buckets_conn_pool_get(buckets_conn_pool_t *pool,
                           const char *host,
                           int port,
                           buckets_connection_t **conn);

/**
 * Release connection back to pool
 * 
 * @param pool Connection pool
 * @param conn Connection to release
 * @return BUCKETS_OK on success
 */
int buckets_conn_pool_release(buckets_conn_pool_t *pool,
                               buckets_connection_t *conn);

/**
 * Close and remove connection from pool
 * 
 * @param pool Connection pool
 * @param conn Connection to close
 * @return BUCKETS_OK on success
 */
int buckets_conn_pool_close(buckets_conn_pool_t *pool,
                             buckets_connection_t *conn);

/**
 * Get pool statistics
 * 
 * @param pool Connection pool
 * @param total Output: total connections
 * @param active Output: active connections
 * @param idle Output: idle connections
 * @return BUCKETS_OK on success
 */
int buckets_conn_pool_stats(buckets_conn_pool_t *pool,
                             int *total,
                             int *active,
                             int *idle);

/**
 * Free connection pool
 * 
 * Closes all connections and frees memory.
 * 
 * @param pool Connection pool
 */
void buckets_conn_pool_free(buckets_conn_pool_t *pool);

/**
 * Send HTTP request over connection
 * 
 * @param conn Connection handle
 * @param method HTTP method (GET, POST, etc.)
 * @param path Request path
 * @param body Request body (optional)
 * @param body_len Body length
 * @param response Output: response string (caller must free)
 * @param status_code Output: HTTP status code
 * @return BUCKETS_OK on success
 */
int buckets_conn_send_request(buckets_connection_t *conn,
                               const char *method,
                               const char *path,
                               const char *body,
                               size_t body_len,
                               char **response,
                               int *status_code);

/* ===================================================================
 * Peer Discovery (Week 33)
 * ===================================================================*/

/**
 * Peer information
 */
typedef struct {
    char node_id[64];          /* Unique node ID (UUID) */
    char endpoint[256];        /* http://host:port or https://host:port */
    bool online;               /* Peer status (true=online, false=offline) */
    time_t last_seen;          /* Last heartbeat timestamp */
} buckets_peer_t;

/**
 * Peer grid (opaque)
 */
typedef struct buckets_peer_grid buckets_peer_grid_t;

/**
 * Create peer grid
 * 
 * @return Peer grid handle or NULL on error
 */
buckets_peer_grid_t* buckets_peer_grid_create(void);

/**
 * Add peer to grid
 * 
 * @param grid Peer grid
 * @param endpoint Peer endpoint (e.g., "http://192.168.1.10:9000")
 * @return BUCKETS_OK on success
 */
int buckets_peer_grid_add_peer(buckets_peer_grid_t *grid,
                                const char *endpoint);

/**
 * Remove peer from grid
 * 
 * @param grid Peer grid
 * @param node_id Node ID to remove
 * @return BUCKETS_OK on success
 */
int buckets_peer_grid_remove_peer(buckets_peer_grid_t *grid,
                                   const char *node_id);

/**
 * Get list of peers
 * 
 * Returns array of peer pointers. Caller must NOT free individual peers.
 * 
 * @param grid Peer grid
 * @param count Output: number of peers
 * @return Array of peer pointers (internal, do not free)
 */
buckets_peer_t** buckets_peer_grid_get_peers(buckets_peer_grid_t *grid,
                                              int *count);

/**
 * Get peer by node ID
 * 
 * @param grid Peer grid
 * @param node_id Node ID
 * @return Peer handle or NULL if not found
 */
buckets_peer_t* buckets_peer_grid_get_peer(buckets_peer_grid_t *grid,
                                            const char *node_id);

/**
 * Update peer last seen timestamp
 * 
 * @param grid Peer grid
 * @param node_id Node ID
 * @param timestamp New timestamp
 * @return BUCKETS_OK on success
 */
int buckets_peer_grid_update_last_seen(buckets_peer_grid_t *grid,
                                        const char *node_id,
                                        time_t timestamp);

/**
 * Free peer grid
 * 
 * @param grid Peer grid
 */
void buckets_peer_grid_free(buckets_peer_grid_t *grid);

/* ===================================================================
 * Health Checking (Week 33)
 * ===================================================================*/

/**
 * Health status change callback
 * 
 * @param node_id Node ID
 * @param online true if online, false if offline
 * @param user_data User data
 */
typedef void (*buckets_health_callback_t)(const char *node_id,
                                           bool online,
                                           void *user_data);

/**
 * Health checker (opaque)
 */
typedef struct buckets_health_checker buckets_health_checker_t;

/**
 * Create health checker
 * 
 * @param grid Peer grid to monitor
 * @param interval_sec Heartbeat interval in seconds
 * @return Health checker handle or NULL on error
 */
buckets_health_checker_t* buckets_health_checker_create(
    buckets_peer_grid_t *grid,
    int interval_sec);

/**
 * Start health checker
 * 
 * Starts background thread that sends heartbeats.
 * 
 * @param checker Health checker
 * @return BUCKETS_OK on success
 */
int buckets_health_checker_start(buckets_health_checker_t *checker);

/**
 * Stop health checker
 * 
 * @param checker Health checker
 * @return BUCKETS_OK on success
 */
int buckets_health_checker_stop(buckets_health_checker_t *checker);

/**
 * Set health status callback
 * 
 * @param checker Health checker
 * @param callback Callback function
 * @param user_data User data passed to callback
 * @return BUCKETS_OK on success
 */
int buckets_health_checker_set_callback(buckets_health_checker_t *checker,
                                         buckets_health_callback_t callback,
                                         void *user_data);

/**
 * Free health checker
 * 
 * Stops checker if running.
 * 
 * @param checker Health checker
 */
void buckets_health_checker_free(buckets_health_checker_t *checker);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_NET_H */
