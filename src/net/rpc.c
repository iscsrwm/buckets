/**
 * RPC Implementation
 * 
 * JSON-based RPC message format and handler registration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <uuid/uuid.h>

#include "buckets.h"
#include "buckets_net.h"
#include "cJSON.h"

/* ===================================================================
 * RPC Context Structure
 * ===================================================================*/

/**
 * RPC handler entry
 */
typedef struct rpc_handler_entry {
    char method[128];                  /* Method name */
    buckets_rpc_handler_t handler;     /* Handler function */
    void *user_data;                   /* User data */
    struct rpc_handler_entry *next;    /* Next in list */
} rpc_handler_entry_t;

/**
 * RPC context
 */
struct buckets_rpc_context {
    buckets_conn_pool_t *pool;         /* Connection pool for RPC calls */
    rpc_handler_entry_t *handlers;     /* Handler list */
    pthread_mutex_t lock;              /* Thread safety */
};

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/**
 * Generate UUID request ID
 */
static void generate_request_id(char *id, size_t len)
{
    (void)len;  /* UUID string is always 37 bytes */
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, id);
}

/**
 * Find handler by method name
 */
static rpc_handler_entry_t* find_handler(buckets_rpc_context_t *ctx, const char *method)
{
    rpc_handler_entry_t *entry = ctx->handlers;
    while (entry) {
        if (strcmp(entry->method, method) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

/* ===================================================================
 * RPC Context API
 * ===================================================================*/

buckets_rpc_context_t* buckets_rpc_context_create(buckets_conn_pool_t *pool)
{
    if (!pool) {
        buckets_error("RPC context create: pool is NULL");
        return NULL;
    }
    
    buckets_rpc_context_t *ctx = buckets_calloc(1, sizeof(buckets_rpc_context_t));
    if (!ctx) {
        return NULL;
    }
    
    ctx->pool = pool;
    ctx->handlers = NULL;
    
    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        buckets_error("RPC context create: failed to initialize mutex");
        buckets_free(ctx);
        return NULL;
    }
    
    return ctx;
}

int buckets_rpc_register_handler(buckets_rpc_context_t *ctx,
                                  const char *method,
                                  buckets_rpc_handler_t handler,
                                  void *user_data)
{
    if (!ctx || !method || !handler) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (strlen(method) >= 128) {
        buckets_error("RPC register handler: method name too long");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&ctx->lock);
    
    /* Check if handler already exists */
    if (find_handler(ctx, method)) {
        pthread_mutex_unlock(&ctx->lock);
        buckets_error("RPC register handler: method '%s' already registered", method);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Create new handler entry */
    rpc_handler_entry_t *entry = buckets_calloc(1, sizeof(rpc_handler_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&ctx->lock);
        return BUCKETS_ERR_NOMEM;
    }
    
    strncpy(entry->method, method, sizeof(entry->method) - 1);
    entry->handler = handler;
    entry->user_data = user_data;
    
    /* Add to list */
    entry->next = ctx->handlers;
    ctx->handlers = entry;
    
    pthread_mutex_unlock(&ctx->lock);
    
    buckets_debug("RPC: Registered handler for method '%s'", method);
    return BUCKETS_OK;
}

void buckets_rpc_context_free(buckets_rpc_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    
    /* Free handler list */
    rpc_handler_entry_t *entry = ctx->handlers;
    while (entry) {
        rpc_handler_entry_t *next = entry->next;
        buckets_free(entry);
        entry = next;
    }
    
    pthread_mutex_destroy(&ctx->lock);
    buckets_free(ctx);
}

/* ===================================================================
 * RPC Request API
 * ===================================================================*/

int buckets_rpc_request_parse(const char *json, buckets_rpc_request_t **request)
{
    if (!json || !request) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        buckets_error("RPC request parse: invalid JSON");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Validate required fields */
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *timestamp = cJSON_GetObjectItem(root, "timestamp");
    
    if (!id || !cJSON_IsString(id) ||
        !method || !cJSON_IsString(method) ||
        !timestamp || !cJSON_IsNumber(timestamp)) {
        cJSON_Delete(root);
        buckets_error("RPC request parse: missing required fields");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Allocate request */
    buckets_rpc_request_t *req = buckets_calloc(1, sizeof(buckets_rpc_request_t));
    if (!req) {
        cJSON_Delete(root);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Copy fields */
    strncpy(req->id, id->valuestring, sizeof(req->id) - 1);
    strncpy(req->method, method->valuestring, sizeof(req->method) - 1);
    req->timestamp = (time_t)timestamp->valueint;
    
    /* Get params (optional) */
    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (params && !cJSON_IsNull(params)) {
        req->params = cJSON_Duplicate(params, 1);
    } else {
        req->params = NULL;
    }
    
    cJSON_Delete(root);
    *request = req;
    return BUCKETS_OK;
}

int buckets_rpc_request_serialize(buckets_rpc_request_t *request, char **json)
{
    if (!request || !json) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return BUCKETS_ERR_NOMEM;
    }
    
    cJSON_AddStringToObject(root, "id", request->id);
    cJSON_AddStringToObject(root, "method", request->method);
    cJSON_AddNumberToObject(root, "timestamp", (double)request->timestamp);
    
    if (request->params) {
        cJSON_AddItemToObject(root, "params", cJSON_Duplicate(request->params, 1));
    } else {
        cJSON_AddNullToObject(root, "params");
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        return BUCKETS_ERR_NOMEM;
    }
    
    *json = json_str;
    return BUCKETS_OK;
}

void buckets_rpc_request_free(buckets_rpc_request_t *request)
{
    if (!request) {
        return;
    }
    
    if (request->params) {
        cJSON_Delete(request->params);
    }
    
    buckets_free(request);
}

/* ===================================================================
 * RPC Response API
 * ===================================================================*/

int buckets_rpc_response_parse(const char *json, buckets_rpc_response_t **response)
{
    if (!json || !response) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        buckets_error("RPC response parse: invalid JSON");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Validate required fields */
    cJSON *id = cJSON_GetObjectItem(root, "id");
    cJSON *timestamp = cJSON_GetObjectItem(root, "timestamp");
    
    if (!id || !cJSON_IsString(id) ||
        !timestamp || !cJSON_IsNumber(timestamp)) {
        cJSON_Delete(root);
        buckets_error("RPC response parse: missing required fields");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Allocate response */
    buckets_rpc_response_t *res = buckets_calloc(1, sizeof(buckets_rpc_response_t));
    if (!res) {
        cJSON_Delete(root);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Copy fields */
    strncpy(res->id, id->valuestring, sizeof(res->id) - 1);
    res->timestamp = (time_t)timestamp->valueint;
    
    /* Get result (optional) */
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (result && !cJSON_IsNull(result)) {
        res->result = cJSON_Duplicate(result, 1);
    } else {
        res->result = NULL;
    }
    
    /* Get error (optional) */
    cJSON *error_code = cJSON_GetObjectItem(root, "error_code");
    cJSON *error_message = cJSON_GetObjectItem(root, "error_message");
    
    if (error_code && cJSON_IsNumber(error_code)) {
        res->error_code = error_code->valueint;
    } else {
        res->error_code = 0;
    }
    
    if (error_message && cJSON_IsString(error_message)) {
        strncpy(res->error_message, error_message->valuestring, 
                sizeof(res->error_message) - 1);
    } else {
        res->error_message[0] = '\0';
    }
    
    cJSON_Delete(root);
    *response = res;
    return BUCKETS_OK;
}

int buckets_rpc_response_serialize(buckets_rpc_response_t *response, char **json)
{
    if (!response || !json) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return BUCKETS_ERR_NOMEM;
    }
    
    cJSON_AddStringToObject(root, "id", response->id);
    cJSON_AddNumberToObject(root, "timestamp", (double)response->timestamp);
    
    if (response->result) {
        cJSON_AddItemToObject(root, "result", cJSON_Duplicate(response->result, 1));
    } else {
        cJSON_AddNullToObject(root, "result");
    }
    
    cJSON_AddNumberToObject(root, "error_code", response->error_code);
    cJSON_AddStringToObject(root, "error_message", response->error_message);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        return BUCKETS_ERR_NOMEM;
    }
    
    *json = json_str;
    return BUCKETS_OK;
}

void buckets_rpc_response_free(buckets_rpc_response_t *response)
{
    if (!response) {
        return;
    }
    
    if (response->result) {
        cJSON_Delete(response->result);
    }
    
    buckets_free(response);
}

/* ===================================================================
 * RPC Call API
 * ===================================================================*/

int buckets_rpc_call(buckets_rpc_context_t *ctx,
                     const char *peer_endpoint,
                     const char *method,
                     cJSON *params,
                     buckets_rpc_response_t **response,
                     int timeout_ms)
{
    struct timespec start_rpc, end_conn, end_send, end_parse;
    clock_gettime(CLOCK_MONOTONIC, &start_rpc);
    
    if (!ctx || !peer_endpoint || !method || !response) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Default timeout: 5 seconds */
    if (timeout_ms <= 0) {
        timeout_ms = 5000;
    }
    
    /* Create RPC request */
    buckets_rpc_request_t request;
    memset(&request, 0, sizeof(request));
    
    generate_request_id(request.id, sizeof(request.id));
    strncpy(request.method, method, sizeof(request.method) - 1);
    request.timestamp = time(NULL);
    request.params = params;
    
    /* Serialize request */
    char *request_json = NULL;
    int ret = buckets_rpc_request_serialize(&request, &request_json);
    if (ret != BUCKETS_OK) {
        return ret;
    }
    
    /* Parse endpoint to get host and port */
    char host[256];
    int port;
    ret = BUCKETS_OK;
    
    const char *proto_end = strstr(peer_endpoint, "://");
    if (!proto_end) {
        buckets_free(request_json);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    const char *host_start = proto_end + 3;
    const char *port_start = strchr(host_start, ':');
    if (!port_start) {
        buckets_free(request_json);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    size_t host_length = port_start - host_start;
    if (host_length >= sizeof(host)) {
        buckets_free(request_json);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    memcpy(host, host_start, host_length);
    host[host_length] = '\0';
    port = atoi(port_start + 1);
    
    /* Get connection from pool */
    buckets_connection_t *conn = NULL;
    ret = buckets_conn_pool_get(ctx->pool, host, port, &conn);
    clock_gettime(CLOCK_MONOTONIC, &end_conn);
    
    if (ret != BUCKETS_OK) {
        buckets_free(request_json);
        buckets_error("RPC call: failed to get connection to %s", peer_endpoint);
        return ret;
    }
    
    /* Send HTTP POST request to /rpc endpoint */
    char *response_body = NULL;
    int status_code = 0;
    ret = buckets_conn_send_request(conn, "POST", "/rpc",
                                     request_json, strlen(request_json),
                                     &response_body, &status_code);
    
    buckets_free(request_json);
    clock_gettime(CLOCK_MONOTONIC, &end_send);
    
    /* Handle connection based on RPC success/failure */
    if (ret != BUCKETS_OK) {
        /* RPC failed - close connection */
        buckets_conn_pool_close(ctx->pool, conn);
        buckets_error("RPC call: failed to send request to %s", peer_endpoint);
        return ret;
    }
    
    if (status_code != 200) {
        buckets_error("RPC call: HTTP %d from %s", status_code, peer_endpoint);
        buckets_free(response_body);
        buckets_conn_pool_close(ctx->pool, conn);
        return BUCKETS_ERR_NETWORK;
    }
    
    /* Parse response */
    buckets_rpc_response_t *res = NULL;
    ret = buckets_rpc_response_parse(response_body, &res);
    buckets_free(response_body);
    clock_gettime(CLOCK_MONOTONIC, &end_parse);
    
    if (ret != BUCKETS_OK) {
        buckets_error("RPC call: failed to parse response from %s", peer_endpoint);
        buckets_conn_pool_close(ctx->pool, conn);
        return ret;
    }
    
    /* Log timing breakdown */
    double conn_time = (end_conn.tv_sec - start_rpc.tv_sec) + 
                      (end_conn.tv_nsec - start_rpc.tv_nsec) / 1e9;
    double send_time = (end_send.tv_sec - end_conn.tv_sec) + 
                      (end_send.tv_nsec - end_conn.tv_nsec) / 1e9;
    double parse_time = (end_parse.tv_sec - end_send.tv_sec) + 
                       (end_parse.tv_nsec - end_send.tv_nsec) / 1e9;
    double total_rpc_time = (end_parse.tv_sec - start_rpc.tv_sec) + 
                           (end_parse.tv_nsec - start_rpc.tv_nsec) / 1e9;
    
    buckets_debug("⏱️  RPC %s: total=%.3fms (conn=%.3fms, send=%.3fms, parse=%.3fms)",
                  method, total_rpc_time * 1000, conn_time * 1000, 
                  send_time * 1000, parse_time * 1000);
    
    /* Success - release connection back to pool for reuse */
    buckets_conn_pool_release(ctx->pool, conn);
    
    *response = res;
    return BUCKETS_OK;
}

/* ===================================================================
 * RPC Dispatch API
 * ===================================================================*/

int buckets_rpc_dispatch(buckets_rpc_context_t *ctx,
                         buckets_rpc_request_t *request,
                         buckets_rpc_response_t **response)
{
    if (!ctx || !request || !response) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&ctx->lock);
    rpc_handler_entry_t *entry = find_handler(ctx, request->method);
    pthread_mutex_unlock(&ctx->lock);
    
    /* Allocate response */
    buckets_rpc_response_t *res = buckets_calloc(1, sizeof(buckets_rpc_response_t));
    if (!res) {
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Copy request ID */
    strncpy(res->id, request->id, sizeof(res->id) - 1);
    res->id[sizeof(res->id) - 1] = '\0';
    res->timestamp = time(NULL);
    
    if (!entry) {
        /* Method not found */
        res->error_code = BUCKETS_ERR_INVALID_ARG;
        snprintf(res->error_message, sizeof(res->error_message),
                 "Method not found: %s", request->method);
        res->result = NULL;
        *response = res;
        return BUCKETS_OK;
    }
    
    /* Call handler */
    cJSON *result = NULL;
    int error_code = 0;
    char error_message[256] = {0};
    
    int ret = entry->handler(request->method, request->params,
                             &result, &error_code, error_message,
                             entry->user_data);
    
    if (ret != BUCKETS_OK) {
        /* Handler failed */
        res->error_code = ret;
        snprintf(res->error_message, sizeof(res->error_message),
                 "Handler failed: %d", ret);
        res->result = NULL;
    } else {
        /* Handler succeeded */
        res->error_code = error_code;
        strncpy(res->error_message, error_message, sizeof(res->error_message) - 1);
        res->error_message[sizeof(res->error_message) - 1] = '\0';
        res->result = result;
    }
    
    *response = res;
    return BUCKETS_OK;
}

/**
 * HTTP Handler for RPC requests
 * 
 * Handles JSON-RPC 2.0 requests over HTTP POST
 */
void buckets_rpc_http_handler(buckets_http_request_t *req,
                               buckets_http_response_t *res)
{
    /* Only accept POST requests */
    if (!req->method || strcmp(req->method, "POST") != 0) {
        buckets_http_response_error(res, 405, "Method not allowed");
        return;
    }
    
    /* Get RPC context from distributed storage */
    extern buckets_rpc_context_t* buckets_distributed_storage_get_rpc_context(void);
    buckets_rpc_context_t *ctx = buckets_distributed_storage_get_rpc_context();
    if (!ctx) {
        buckets_http_response_error(res, 500, "RPC not initialized");
        return;
    }
    
    /* Parse JSON-RPC request */
    if (!req->body || req->body_len == 0) {
        buckets_http_response_error(res, 400, "Empty request");
        return;
    }
    
    cJSON *json = cJSON_ParseWithLength(req->body, req->body_len);
    if (!json) {
        buckets_http_response_error(res, 400, "Invalid JSON");
        return;
    }
    
    /* Extract method */
    cJSON *method_obj = cJSON_GetObjectItem(json, "method");
    if (!method_obj || !cJSON_IsString(method_obj)) {
        cJSON_Delete(json);
        buckets_http_response_error(res, 400, "Missing 'method'");
        return;
    }
    
    const char *method = method_obj->valuestring;
    cJSON *params = cJSON_GetObjectItem(json, "params");
    cJSON *id_obj = cJSON_GetObjectItem(json, "id");
    
    if (!params) {
        cJSON_Delete(json);
        buckets_http_response_error(res, 400, "Missing 'params'");
        return;
    }
    
    /* Find handler */
    pthread_mutex_lock(&ctx->lock);
    rpc_handler_entry_t *entry = ctx->handlers;
    while (entry) {
        if (strcmp(entry->method, method) == 0) {
            break;
        }
        entry = entry->next;
    }
    pthread_mutex_unlock(&ctx->lock);
    
    if (!entry) {
        cJSON_Delete(json);
        buckets_http_response_error(res, 404, "Method not found");
        return;
    }
    
    /* Call handler */
    cJSON *result_obj = NULL;
    int error_code = 0;
    char error_message[256] = {0};
    
    int ret = entry->handler(method, params, &result_obj, &error_code, 
                            error_message, entry->user_data);
    
    /* Build RPC response in internal format */
    cJSON *response = cJSON_CreateObject();
    
    /* Add ID - convert to string if needed */
    if (id_obj) {
        if (cJSON_IsString(id_obj)) {
            cJSON_AddStringToObject(response, "id", id_obj->valuestring);
        } else if (cJSON_IsNumber(id_obj)) {
            char id_str[32];
            snprintf(id_str, sizeof(id_str), "%d", id_obj->valueint);
            cJSON_AddStringToObject(response, "id", id_str);
        } else {
            cJSON_AddStringToObject(response, "id", "unknown");
        }
    } else {
        cJSON_AddStringToObject(response, "id", "unknown");
    }
    
    /* Add timestamp */
    cJSON_AddNumberToObject(response, "timestamp", (double)time(NULL));
    
    /* Add result */
    if (ret == BUCKETS_OK && result_obj) {
        cJSON_AddItemToObject(response, "result", result_obj);
    } else {
        cJSON_AddNullToObject(response, "result");
        if (result_obj) {
            cJSON_Delete(result_obj);
        }
    }
    
    /* Add error fields */
    cJSON_AddNumberToObject(response, "error_code", error_code ? error_code : ret);
    cJSON_AddStringToObject(response, "error_message", 
                           error_message[0] ? error_message : (ret == BUCKETS_OK ? "" : "RPC call failed"));
    
    /* Convert to JSON string and send */
    char *response_str = cJSON_PrintUnformatted(response);
    buckets_http_response_json(res, 200, response_str);
    
    buckets_free(response_str);
    cJSON_Delete(response);
    cJSON_Delete(json);
}
