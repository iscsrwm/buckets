/**
 * HTTP Server Implementation
 * 
 * Wraps mongoose library to provide HTTP/S server functionality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "buckets.h"
#include "buckets_net.h"
#include "../../third_party/mongoose/mongoose.h"

/* ===================================================================
 * HTTP Server Structure
 * ===================================================================*/

struct buckets_http_server {
    struct mg_mgr mgr;              /* Mongoose manager */
    struct mg_connection *conn;     /* Listening connection */
    char address[256];              /* Bind address */
    int port;                       /* Port number */
    char url[512];                  /* Full URL (http://addr:port) */
    
    buckets_http_handler_t default_handler;  /* Default handler */
    void *default_handler_data;              /* User data for default handler */
    buckets_router_t *router;                /* Router for path matching */
    
    pthread_t thread;               /* Server thread */
    bool running;                   /* Server running flag */
    pthread_mutex_t lock;           /* Thread safety */
};

/* ===================================================================
 * Forward Declarations
 * ===================================================================*/

static void* server_thread_main(void *arg);
static void mg_event_handler(struct mg_connection *c, int ev, void *ev_data);

/* ===================================================================
 * HTTP Server API
 * ===================================================================*/

buckets_http_server_t* buckets_http_server_create(const char *addr, int port)
{
    if (!addr || port <= 0 || port > 65535) {
        buckets_error("Invalid address or port");
        return NULL;
    }
    
    buckets_http_server_t *server = buckets_calloc(1, sizeof(buckets_http_server_t));
    if (!server) {
        return NULL;
    }
    
    /* Store address and port */
    strncpy(server->address, addr, sizeof(server->address) - 1);
    server->port = port;
    snprintf(server->url, sizeof(server->url), "http://%s:%d", addr, port);
    
    /* Initialize mongoose */
    mg_mgr_init(&server->mgr);
    
    /* Initialize mutex */
    pthread_mutex_init(&server->lock, NULL);
    
    server->running = false;
    server->default_handler = NULL;
    server->default_handler_data = NULL;
    server->router = NULL;
    
    buckets_info("Created HTTP server: %s", server->url);
    
    return server;
}

int buckets_http_server_set_default_handler(buckets_http_server_t *server,
                                              buckets_http_handler_t handler,
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

int buckets_http_server_start(buckets_http_server_t *server)
{
    if (!server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&server->lock);
    
    if (server->running) {
        pthread_mutex_unlock(&server->lock);
        return BUCKETS_ERR_INVALID_ARG;  /* Already running */
    }
    
    /* Start mongoose listening */
    server->conn = mg_http_listen(&server->mgr, server->url, 
                                   mg_event_handler, server);
    if (!server->conn) {
        pthread_mutex_unlock(&server->lock);
        buckets_error("Failed to start HTTP server on %s", server->url);
        return BUCKETS_ERR_IO;
    }
    
    /* Start server thread */
    server->running = true;
    int ret = pthread_create(&server->thread, NULL, server_thread_main, server);
    if (ret != 0) {
        server->running = false;
        pthread_mutex_unlock(&server->lock);
        buckets_error("Failed to create server thread: %d", ret);
        return BUCKETS_ERR_IO;
    }
    
    pthread_mutex_unlock(&server->lock);
    
    buckets_info("HTTP server started: %s", server->url);
    
    return BUCKETS_OK;
}

int buckets_http_server_stop(buckets_http_server_t *server)
{
    if (!server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&server->lock);
    
    if (!server->running) {
        pthread_mutex_unlock(&server->lock);
        return BUCKETS_OK;  /* Already stopped */
    }
    
    server->running = false;
    pthread_mutex_unlock(&server->lock);
    
    /* Wait for thread to finish */
    pthread_join(server->thread, NULL);
    
    buckets_info("HTTP server stopped: %s", server->url);
    
    return BUCKETS_OK;
}

void buckets_http_server_free(buckets_http_server_t *server)
{
    if (!server) {
        return;
    }
    
    /* Stop if running */
    if (server->running) {
        buckets_http_server_stop(server);
    }
    
    /* Free mongoose manager */
    mg_mgr_free(&server->mgr);
    
    /* Destroy mutex */
    pthread_mutex_destroy(&server->lock);
    
    buckets_info("Freed HTTP server: %s", server->url);
    
    buckets_free(server);
}

int buckets_http_server_get_address(buckets_http_server_t *server,
                                      char *addr, size_t len)
{
    if (!server || !addr || len == 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    snprintf(addr, len, "%s", server->url);
    return BUCKETS_OK;
}

/* ===================================================================
 * Internal Implementation
 * ===================================================================*/

/**
 * Server thread main function
 */
static void* server_thread_main(void *arg)
{
    buckets_http_server_t *server = (buckets_http_server_t*)arg;
    
    buckets_debug("Server thread started");
    
    while (server->running) {
        pthread_mutex_lock(&server->lock);
        mg_mgr_poll(&server->mgr, 100);  /* Poll with 100ms timeout */
        pthread_mutex_unlock(&server->lock);
    }
    
    buckets_debug("Server thread stopped");
    
    return NULL;
}

/**
 * Mongoose event handler
 */
static void mg_event_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG) {
        buckets_http_server_t *server = (buckets_http_server_t*)c->fn_data;
        struct mg_http_message *hm = (struct mg_http_message*)ev_data;
        
        /* Create request structure */
        buckets_http_request_t req;
        req.method = hm->method.buf ? strndup(hm->method.buf, hm->method.len) : NULL;
        req.uri = hm->uri.buf ? strndup(hm->uri.buf, hm->uri.len) : NULL;
        req.query_string = hm->query.buf ? strndup(hm->query.buf, hm->query.len) : NULL;
        req.body = hm->body.buf ? strndup(hm->body.buf, hm->body.len) : NULL;
        req.body_len = hm->body.len;
        req.internal = hm;
        
        /* Create response structure */
        buckets_http_response_t res;
        res.status_code = 200;
        res.body = NULL;
        res.body_len = 0;
        res.headers = NULL;
        
        /* Try router first if available */
        bool handled = false;
        if (server->router && req.method && req.uri) {
            buckets_route_match_t match;
            if (buckets_router_match(server->router, req.method, req.uri, &match) == BUCKETS_OK) {
                if (match.matched && match.handler) {
                    match.handler(&req, &res, match.user_data);
                    handled = true;
                }
            }
        }
        
        /* Fall back to default handler */
        if (!handled && server->default_handler) {
            server->default_handler(&req, &res, server->default_handler_data);
            handled = true;
        }
        
        /* Send response */
        if (handled) {
            mg_http_reply(c, res.status_code, res.headers ? res.headers : "",
                          res.body ? "%.*s" : "", (int)res.body_len, res.body);
        } else {
            /* No handler - send 404 */
            mg_http_reply(c, 404, "Content-Type: text/plain\r\n",
                          "404 Not Found");
        }
        
        /* Cleanup */
        if (req.method) free((void*)req.method);
        if (req.uri) free((void*)req.uri);
        if (req.query_string) free((void*)req.query_string);
        if (req.body) free((void*)req.body);
        if (res.body) free(res.body);
        if (res.headers) free(res.headers);
    }
}

/* ===================================================================
 * HTTP Response Helpers
 * ===================================================================*/

void buckets_http_response_set(buckets_http_response_t *res,
                                 int status_code,
                                 const char *body,
                                 size_t body_len)
{
    if (!res) {
        return;
    }
    
    res->status_code = status_code;
    
    if (body && body_len > 0) {
        res->body = buckets_malloc(body_len + 1);
        if (res->body) {
            memcpy(res->body, body, body_len);
            res->body[body_len] = '\0';
            res->body_len = body_len;
        }
    } else {
        res->body = NULL;
        res->body_len = 0;
    }
}

void buckets_http_response_set_header(buckets_http_response_t *res,
                                        const char *name,
                                        const char *value)
{
    if (!res || !name || !value) {
        return;
    }
    
    /* Allocate or reallocate headers buffer */
    size_t needed = strlen(name) + strlen(value) + 4;  /* "Name: Value\r\n" */
    
    if (res->headers) {
        size_t current_len = strlen(res->headers);
        char *new_headers = buckets_realloc(res->headers, current_len + needed + 1);
        if (new_headers) {
            res->headers = new_headers;
            snprintf(res->headers + current_len, needed + 1, "%s: %s\r\n", name, value);
        }
    } else {
        res->headers = buckets_malloc(needed + 1);
        if (res->headers) {
            snprintf(res->headers, needed + 1, "%s: %s\r\n", name, value);
        }
    }
}

void buckets_http_response_json(buckets_http_response_t *res,
                                  int status_code,
                                  const char *json)
{
    if (!res || !json) {
        return;
    }
    
    buckets_http_response_set_header(res, "Content-Type", "application/json");
    buckets_http_response_set(res, status_code, json, strlen(json));
}

void buckets_http_response_error(buckets_http_response_t *res,
                                   int status_code,
                                   const char *message)
{
    if (!res) {
        return;
    }
    
    /* Create JSON error response */
    char json[512];
    snprintf(json, sizeof(json), 
             "{\"error\":\"%s\",\"status\":%d}", 
             message ? message : "Unknown error",
             status_code);
    
    buckets_http_response_json(res, status_code, json);
}
