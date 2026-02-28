/**
 * HTTP Server Implementation (libuv-based)
 * 
 * Implements the buckets_http_server_* API using the UV HTTP server.
 * This replaces the mongoose-based implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "buckets.h"
#include "buckets_net.h"
#include "uv_server_internal.h"

/* ===================================================================
 * HTTP Server Structure
 * ===================================================================*/

struct buckets_http_server {
    uv_http_server_t *uv_server;    /* Underlying UV server */
    char address[256];              /* Bind address */
    int port;                       /* Port number */
    char url[512];                  /* Full URL (http://addr:port) */
    
    buckets_http_handler_t default_handler;  /* Default handler */
    void *default_handler_data;              /* User data for default handler */
    buckets_router_t *router;                /* Router for path matching */
    
    /* TLS configuration */
    bool tls_enabled;               /* TLS enabled flag */
    char cert_file[512];            /* Certificate file path */
    char key_file[512];             /* Private key file path */
};

/* ===================================================================
 * Handler Wrapper
 * ===================================================================*/

/* Global pointer to current server for handler lookup (thread-local would be better) */
static __thread buckets_http_server_t *g_current_server = NULL;

/**
 * UV handler that wraps the buckets_http_handler_t interface
 */
static void uv_handler_wrapper(uv_http_conn_t *conn, void *user_data)
{
    buckets_http_server_t *server = (buckets_http_server_t*)user_data;
    if (!server) {
        server = g_current_server;
    }
    
    /* Build buckets_http_request_t from connection */
    buckets_http_request_t http_req;
    memset(&http_req, 0, sizeof(http_req));
    
    http_req.method = llhttp_method_name(llhttp_get_method(&conn->parser));
    http_req.uri = conn->url;
    http_req.query_string = strchr(conn->url, '?');
    http_req.body = conn->body;
    http_req.body_len = conn->body_len;
    http_req.internal = conn;
    
    /* Create response structure */
    buckets_http_response_t http_res;
    memset(&http_res, 0, sizeof(http_res));
    
    /* Try router first */
    buckets_http_handler_t handler = NULL;
    void *handler_data = NULL;
    
    if (server && server->router) {
        buckets_route_match_t match;
        if (buckets_router_match(server->router, http_req.method, 
                                  http_req.uri, &match) == BUCKETS_OK && match.matched) {
            handler = match.handler;
            handler_data = match.user_data;
        }
    }
    
    /* Fall back to default handler */
    if (!handler && server && server->default_handler) {
        handler = server->default_handler;
        handler_data = server->default_handler_data;
    }
    
    if (handler) {
        handler(&http_req, &http_res, handler_data);
    } else {
        /* No handler - 404 */
        http_res.status_code = 404;
        http_res.body = buckets_strdup("Not Found");
        http_res.body_len = 9;
    }
    
    /* Send response */
    if (http_res.status_code > 0) {
        /* Parse all headers from the headers string */
        /* Format: "Name1: Value1\r\nName2: Value2\r\n..." */
        const char *headers[32];  /* Up to 16 header name/value pairs */
        int header_count = 0;
        static __thread char header_values[16][256];  /* Thread-local storage for values */
        static __thread char header_names[16][64];    /* Thread-local storage for names */
        int value_idx = 0;
        
        bool has_content_type = false;
        
        if (http_res.headers) {
            const char *line = http_res.headers;
            while (*line && value_idx < 16 && header_count < 30) {
                /* Find colon */
                const char *colon = strchr(line, ':');
                if (!colon) break;
                
                /* Extract header name */
                size_t name_len = (size_t)(colon - line);
                if (name_len >= sizeof(header_names[0])) name_len = sizeof(header_names[0]) - 1;
                strncpy(header_names[value_idx], line, name_len);
                header_names[value_idx][name_len] = '\0';
                
                /* Skip colon and whitespace */
                const char *value = colon + 1;
                while (*value == ' ') value++;
                
                /* Find end of value */
                const char *end = strchr(value, '\r');
                if (!end) end = strchr(value, '\n');
                if (!end) end = value + strlen(value);
                
                /* Extract value */
                size_t val_len = (size_t)(end - value);
                if (val_len >= sizeof(header_values[0])) val_len = sizeof(header_values[0]) - 1;
                strncpy(header_values[value_idx], value, val_len);
                header_values[value_idx][val_len] = '\0';
                
                /* Add to headers array */
                headers[header_count++] = header_names[value_idx];
                headers[header_count++] = header_values[value_idx];
                
                if (strcasecmp(header_names[value_idx], "Content-Type") == 0) {
                    has_content_type = true;
                }
                
                value_idx++;
                
                /* Move to next line */
                line = end;
                while (*line == '\r' || *line == '\n') line++;
            }
        }
        
        /* Add default Content-Type if not set */
        if (!has_content_type && http_res.body_len > 0) {
            headers[header_count++] = "Content-Type";
            headers[header_count++] = "application/xml";
        }
        
        headers[header_count] = NULL;
        
        uv_http_response_start(conn, http_res.status_code, headers, header_count, 
                               http_res.body_len);
        
        if (http_res.body && http_res.body_len > 0) {
            /* Write in chunks for large responses */
            const size_t CHUNK_SIZE = 64 * 1024;
            size_t offset = 0;
            
            while (offset < http_res.body_len) {
                size_t remaining = http_res.body_len - offset;
                size_t write_size = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
                uv_http_response_write(conn, (char*)http_res.body + offset, write_size);
                offset += write_size;
            }
        }
        
        /* Always end the response (sends terminating chunk for chunked encoding) */
        uv_http_response_end(conn);
    } else {
        uv_http_response_start(conn, 500, NULL, 0, 0);
        uv_http_response_end(conn);
    }
    
    /* Free response */
    if (http_res.body) {
        buckets_free(http_res.body);
    }
    if (http_res.headers) {
        buckets_free(http_res.headers);
    }
}

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
    
    /* Create UV server */
    server->uv_server = uv_http_server_create(addr, port);
    if (!server->uv_server) {
        buckets_free(server);
        return NULL;
    }
    
    /* Set default handler wrapper */
    uv_http_server_set_handler(server->uv_server, uv_handler_wrapper, server);
    
    server->default_handler = NULL;
    server->default_handler_data = NULL;
    server->router = NULL;
    
    return server;
}

int buckets_http_server_set_default_handler(buckets_http_server_t *server,
                                             buckets_http_handler_t handler,
                                             void *user_data)
{
    if (!server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    server->default_handler = handler;
    server->default_handler_data = user_data;
    
    return BUCKETS_OK;
}

int buckets_http_server_set_router(buckets_http_server_t *server,
                                    buckets_router_t *router)
{
    if (!server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    server->router = router;
    
    return BUCKETS_OK;
}

buckets_router_t* buckets_http_server_get_router(buckets_http_server_t *server)
{
    if (!server) {
        return NULL;
    }
    
    return server->router;
}

int buckets_http_server_start(buckets_http_server_t *server)
{
    if (!server || !server->uv_server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Set global server for handler lookup */
    g_current_server = server;
    
    return uv_http_server_start(server->uv_server);
}

int buckets_http_server_stop(buckets_http_server_t *server)
{
    if (!server || !server->uv_server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    return uv_http_server_stop(server->uv_server);
}

void buckets_http_server_free(buckets_http_server_t *server)
{
    if (!server) {
        return;
    }
    
    if (server->uv_server) {
        uv_http_server_free(server->uv_server);
    }
    
    buckets_free(server);
}

int buckets_http_server_get_address(buckets_http_server_t *server,
                                     char *addr, size_t len)
{
    if (!server || !addr || len == 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    strncpy(addr, server->url, len - 1);
    addr[len - 1] = '\0';
    
    return BUCKETS_OK;
}

int buckets_http_server_enable_tls(buckets_http_server_t *server,
                                    buckets_tls_config_t *config)
{
    if (!server || !server->uv_server || !config) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (!config->cert_file || !config->key_file) {
        buckets_error("TLS requires certificate and key files");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    server->tls_enabled = true;
    strncpy(server->cert_file, config->cert_file, sizeof(server->cert_file) - 1);
    strncpy(server->key_file, config->key_file, sizeof(server->key_file) - 1);
    
    /* Enable TLS on UV server */
    return uv_http_server_enable_tls(server->uv_server, 
                                      config->cert_file, 
                                      config->key_file);
}

/* ===================================================================
 * Response Helpers (same as before, for compatibility)
 * ===================================================================*/

void buckets_http_response_set(buckets_http_response_t *res,
                                int status_code,
                                const char *body,
                                size_t body_len)
{
    if (!res) return;
    
    res->status_code = status_code;
    
    if (body && body_len > 0) {
        res->body = buckets_malloc(body_len);
        if (res->body) {
            memcpy(res->body, body, body_len);
            res->body_len = body_len;
        }
    }
}

void buckets_http_response_set_header(buckets_http_response_t *res,
                                       const char *name,
                                       const char *value)
{
    if (!res || !name || !value) return;
    
    size_t header_len = strlen(name) + strlen(value) + 4; /* ": \r\n" */
    size_t existing_len = res->headers ? strlen(res->headers) : 0;
    
    char *new_headers = buckets_malloc(existing_len + header_len + 1);
    if (!new_headers) return;
    
    if (res->headers) {
        strcpy(new_headers, res->headers);
        buckets_free(res->headers);
    } else {
        new_headers[0] = '\0';
    }
    
    sprintf(new_headers + existing_len, "%s: %s\r\n", name, value);
    res->headers = new_headers;
}

void buckets_http_response_json(buckets_http_response_t *res,
                                 int status_code,
                                 const char *json)
{
    if (!res || !json) return;
    
    buckets_http_response_set_header(res, "Content-Type", "application/json");
    buckets_http_response_set(res, status_code, json, strlen(json));
}

void buckets_http_response_error(buckets_http_response_t *res,
                                  int status_code,
                                  const char *message)
{
    if (!res) return;
    
    char body[1024];
    int len = snprintf(body, sizeof(body),
        "{\"error\": {\"code\": %d, \"message\": \"%s\"}}",
        status_code, message ? message : "Unknown error");
    
    buckets_http_response_json(res, status_code, body);
    (void)len;
}
