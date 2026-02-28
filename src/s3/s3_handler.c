/**
 * S3 Request Handler
 * 
 * Parses S3 requests and routes to operation handlers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "buckets.h"
#include "buckets_s3.h"

/* ===================================================================
 * Request Parsing
 * ===================================================================*/

/**
 * Parse S3 path: /bucket/key or /bucket or /
 */
static int parse_s3_path(const char *uri, buckets_s3_request_t *req)
{
    if (!uri || !req) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Skip leading slash */
    if (uri[0] == '/') {
        uri++;
    }
    
    /* Empty path = list buckets */
    if (uri[0] == '\0') {
        req->bucket[0] = '\0';
        req->key[0] = '\0';
        return BUCKETS_OK;
    }
    
    /* Find first slash (separates bucket from key) and query string */
    const char *slash = strchr(uri, '/');
    const char *question = strchr(uri, '?');
    
    if (!slash) {
        /* Just bucket, no key */
        size_t bucket_len = question ? (size_t)(question - uri) : strlen(uri);
        if (bucket_len >= sizeof(req->bucket)) {
            return BUCKETS_ERR_INVALID_ARG;
        }
        strncpy(req->bucket, uri, bucket_len);
        req->bucket[bucket_len] = '\0';
        req->key[0] = '\0';
        return BUCKETS_OK;
    }
    
    /* Extract bucket */
    size_t bucket_len = slash - uri;
    if (bucket_len >= sizeof(req->bucket)) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    strncpy(req->bucket, uri, bucket_len);
    req->bucket[bucket_len] = '\0';
    
    /* Extract key (everything after first slash, up to query string) */
    const char *key = slash + 1;
    size_t key_len = question ? (size_t)(question - key) : strlen(key);
    if (key_len >= sizeof(req->key)) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    strncpy(req->key, key, key_len);
    req->key[key_len] = '\0';
    
    return BUCKETS_OK;
}

/**
 * URL decode a string in-place
 * Converts %XX hex codes to characters
 */
static void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16*a+b;
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

/**
 * Get header value from HTTP request
 */
static const char* get_header(buckets_http_request_t *http_req, const char *name)
{
    /* This is a simplified implementation */
    /* In a real HTTP server, we'd parse all headers properly */
    /* For now, mongoose provides limited header access */
    (void)http_req;
    (void)name;
    return NULL;
}

/* ===================================================================
 * S3 Request/Response Management
 * ===================================================================*/

int buckets_s3_parse_request(buckets_http_request_t *http_req,
                              buckets_s3_request_t **s3_req)
{
    if (!http_req || !s3_req) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Allocate S3 request */
    buckets_s3_request_t *req = buckets_calloc(1, sizeof(buckets_s3_request_t));
    if (!req) {
        return BUCKETS_ERR_NOMEM;
    }
    
    req->http_req = http_req;
    
    /* Parse URI to extract bucket and key */
    int ret = parse_s3_path(http_req->uri, req);
    if (ret != BUCKETS_OK) {
        buckets_free(req);
        return ret;
    }
    
    /* Copy body pointer (not owned by S3 request) */
    req->body = http_req->body;
    req->body_len = http_req->body_len;
    
    /* Parse query string (simple implementation) */
    const char *query = http_req->query_string;
    if (query && query[0] != '\0') {
        /* Skip leading '?' if present */
        if (query[0] == '?') {
            query++;
        }
        /* Count parameters */
        int count = 1;
        for (const char *p = query; *p; p++) {
            if (*p == '&') count++;
        }
        
        /* Allocate arrays */
        req->query_params_keys = buckets_calloc(count, sizeof(char*));
        req->query_params_values = buckets_calloc(count, sizeof(char*));
        req->query_count = 0;
        
        if (req->query_params_keys && req->query_params_values) {
            /* Parse key=value pairs */
            char *query_copy = buckets_strdup(query);
            char *saveptr;
            char *pair = strtok_r(query_copy, "&", &saveptr);
            
            while (pair && req->query_count < count) {
                char *equals = strchr(pair, '=');
                char decoded_key[256];
                char decoded_value[1024];
                
                if (equals) {
                    /* Key=Value format */
                    *equals = '\0';
                    url_decode(decoded_key, pair);
                    url_decode(decoded_value, equals + 1);
                } else {
                    /* Key only (no value), e.g., ?uploads */
                    url_decode(decoded_key, pair);
                    decoded_value[0] = '\0';  /* Empty value */
                }
                
                req->query_params_keys[req->query_count] = buckets_strdup(decoded_key);
                req->query_params_values[req->query_count] = buckets_strdup(decoded_value);
                req->query_count++;
                
                pair = strtok_r(NULL, "&", &saveptr);
            }
            
            buckets_free(query_copy);
        }
    }
    
    /* Extract headers */
    const char *content_type = get_header(http_req, "Content-Type");
    if (content_type) {
        strncpy(req->content_type, content_type, sizeof(req->content_type) - 1);
        req->content_type[sizeof(req->content_type) - 1] = '\0';
    }
    
    req->content_length = http_req->body_len;
    
    /* TODO: Parse Authorization header */
    /* For now, allow unauthenticated requests for testing */
    
    *s3_req = req;
    return BUCKETS_OK;
}

void buckets_s3_request_free(buckets_s3_request_t *req)
{
    if (!req) {
        return;
    }
    
    /* Free query params if allocated */
    if (req->query_params_keys) {
        for (int i = 0; i < req->query_count; i++) {
            buckets_free(req->query_params_keys[i]);
            buckets_free(req->query_params_values[i]);
        }
        buckets_free(req->query_params_keys);
        buckets_free(req->query_params_values);
    }
    
    buckets_free(req);
}

void buckets_s3_response_free(buckets_s3_response_t *res)
{
    if (!res) {
        return;
    }
    
    /* Free body if allocated */
    if (res->body) {
        buckets_free(res->body);
    }
    
    buckets_free(res);
}

/* ===================================================================
 * S3 Handler
 * ===================================================================*/

/**
 * Check if query parameter exists
 */
static bool has_query_param(buckets_s3_request_t *req, const char *key)
{
    for (int i = 0; i < req->query_count; i++) {
        if (req->query_params_keys[i] && strcmp(req->query_params_keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}

void buckets_s3_handler(buckets_http_request_t *req,
                        buckets_http_response_t *res,
                        void *user_data)
{
    (void)user_data;
    
    /* Check for RPC endpoint */
    if (req->uri && strcmp(req->uri, "/rpc") == 0) {
        buckets_info("RPC request received: method=%s, uri=%s, body_len=%zu",
                     req->method ? req->method : "NULL", req->uri, req->body_len);
        /* Forward to RPC handler */
        extern void buckets_rpc_http_handler(buckets_http_request_t *req,
                                             buckets_http_response_t *res);
        buckets_rpc_http_handler(req, res);
        return;
    }
    
    /* Parse S3 request */
    buckets_s3_request_t *s3_req = NULL;
    int ret = buckets_s3_parse_request(req, &s3_req);
    if (ret != BUCKETS_OK) {
        buckets_http_response_error(res, 400, "Invalid S3 request");
        return;
    }
    
    /* Allocate S3 response */
    buckets_s3_response_t *s3_res = buckets_calloc(1, sizeof(buckets_s3_response_t));
    if (!s3_res) {
        buckets_s3_request_free(s3_req);
        buckets_http_response_error(res, 500, "Internal error");
        return;
    }
    
    /* Route based on method and path */
    const char *method = req->method;
    
    if (strcmp(method, "PUT") == 0) {
        if (s3_req->bucket[0] != '\0' && s3_req->key[0] != '\0') {
            /* Check for multipart upload part */
            if (has_query_param(s3_req, "uploadId") && has_query_param(s3_req, "partNumber")) {
                /* PUT /{bucket}/{key}?uploadId={id}&partNumber={n} - Upload part */
                ret = buckets_s3_upload_part(s3_req, s3_res);
            } else {
                /* PUT object */
                ret = buckets_s3_put_object(s3_req, s3_res);
            }
        } else if (s3_req->bucket[0] != '\0') {
            /* PUT bucket (create bucket) */
            ret = buckets_s3_put_bucket(s3_req, s3_res);
        } else {
            buckets_s3_xml_error(s3_res, "InvalidRequest",
                                "Invalid PUT request", "/");
        }
    } else if (strcmp(method, "GET") == 0) {
        if (s3_req->bucket[0] != '\0' && s3_req->key[0] != '\0') {
            /* Check for multipart list parts */
            if (has_query_param(s3_req, "uploadId")) {
                /* GET /{bucket}/{key}?uploadId={id} - List parts */
                ret = buckets_s3_list_parts(s3_req, s3_res);
            } else {
                /* GET object */
                ret = buckets_s3_get_object(s3_req, s3_res);
            }
        } else if (s3_req->bucket[0] != '\0') {
            /* LIST objects - check for list-type query parameter */
            /* If list-type=2, use v2 API, otherwise use v1 */
            const char *list_type = NULL;
            for (int i = 0; i < s3_req->query_count; i++) {
                if (s3_req->query_params_keys[i] &&
                    strcmp(s3_req->query_params_keys[i], "list-type") == 0) {
                    list_type = s3_req->query_params_values[i];
                    break;
                }
            }
            
            if (list_type && strcmp(list_type, "2") == 0) {
                ret = buckets_s3_list_objects_v2(s3_req, s3_res);
            } else {
                ret = buckets_s3_list_objects_v1(s3_req, s3_res);
            }
        } else {
            /* LIST buckets */
            ret = buckets_s3_list_buckets(s3_req, s3_res);
        }
    } else if (strcmp(method, "DELETE") == 0) {
        if (s3_req->bucket[0] != '\0' && s3_req->key[0] != '\0') {
            /* Check for multipart abort */
            if (has_query_param(s3_req, "uploadId")) {
                /* DELETE /{bucket}/{key}?uploadId={id} - Abort multipart upload */
                ret = buckets_s3_abort_multipart_upload(s3_req, s3_res);
            } else {
                /* DELETE object */
                ret = buckets_s3_delete_object(s3_req, s3_res);
            }
        } else if (s3_req->bucket[0] != '\0') {
            /* DELETE bucket */
            ret = buckets_s3_delete_bucket(s3_req, s3_res);
        } else {
            buckets_s3_xml_error(s3_res, "InvalidRequest",
                                "Invalid DELETE request", "/");
        }
    } else if (strcmp(method, "HEAD") == 0) {
        if (s3_req->bucket[0] != '\0' && s3_req->key[0] != '\0') {
            /* HEAD object */
            ret = buckets_s3_head_object(s3_req, s3_res);
        } else if (s3_req->bucket[0] != '\0') {
            /* HEAD bucket */
            ret = buckets_s3_head_bucket(s3_req, s3_res);
        } else {
            buckets_s3_xml_error(s3_res, "InvalidRequest",
                                "Invalid HEAD request", "/");
        }
    } else if (strcmp(method, "POST") == 0) {
        if (s3_req->bucket[0] != '\0' && s3_req->key[0] != '\0') {
            /* Multipart upload operations */
            if (has_query_param(s3_req, "uploads")) {
                /* POST /{bucket}/{key}?uploads - Initiate multipart upload */
                ret = buckets_s3_initiate_multipart_upload(s3_req, s3_res);
            } else if (has_query_param(s3_req, "uploadId")) {
                /* POST /{bucket}/{key}?uploadId={id} - Complete multipart upload */
                ret = buckets_s3_complete_multipart_upload(s3_req, s3_res);
            } else {
                buckets_s3_xml_error(s3_res, "InvalidRequest",
                                    "Invalid POST request", req->uri);
            }
        } else {
            buckets_s3_xml_error(s3_res, "InvalidRequest",
                                "Invalid POST request", "/");
        }
    } else {
        buckets_s3_xml_error(s3_res, "MethodNotAllowed",
                            "Method not allowed", req->uri);
    }
    
    /* Convert S3 response to HTTP response */
    res->status_code = s3_res->status_code;
    
    if (s3_res->body) {
        res->body = s3_res->body;
        res->body_len = s3_res->body_len;
        s3_res->body = NULL;  /* Transfer ownership */
    }
    
    /* Set headers */
    if (s3_res->etag[0] != '\0') {
        buckets_http_response_set_header(res, "ETag", s3_res->etag);
    }
    
    if (s3_res->last_modified[0] != '\0') {
        buckets_http_response_set_header(res, "Last-Modified", s3_res->last_modified);
    }
    
    if (s3_res->content_type[0] != '\0') {
        buckets_http_response_set_header(res, "Content-Type", s3_res->content_type);
    } else {
        buckets_http_response_set_header(res, "Content-Type", "application/xml");
    }
    
    /* For HEAD requests, set body_len from s3_res->content_length 
     * so the HTTP layer uses it for Content-Length header.
     * We don't send body for HEAD, but Content-Length should be object size. */
    if (strcmp(method, "HEAD") == 0 && s3_res->content_length > 0) {
        res->body_len = (size_t)s3_res->content_length;
    }
    
    /* Cleanup */
    buckets_s3_response_free(s3_res);
    buckets_s3_request_free(s3_req);
}

/* ===================================================================
 * Initialization
 * ===================================================================*/

int buckets_s3_init(buckets_http_server_t *server)
{
    if (!server) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Register S3 handler for all paths */
    /* In a real implementation, we'd use a router */
    /* For now, this is a placeholder */
    buckets_debug("S3 API initialized");
    
    return BUCKETS_OK;
}
