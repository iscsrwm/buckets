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

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_NET_H */
