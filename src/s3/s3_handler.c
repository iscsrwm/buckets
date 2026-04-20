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

/* Forward declaration */
static void url_decode_inplace(char *str);

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
    
    /* URL-decode bucket and key - they come encoded from the HTTP layer
     * but storage operations expect decoded names. This matches the
     * streaming handler which also decodes before storing objects. */
    url_decode_inplace(req->bucket);
    url_decode_inplace(req->key);
    
    return BUCKETS_OK;
}

/**
 * URL decode a string into destination buffer
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
 * URL decode a string in-place
 * Safe because decoded string is always <= original length
 */
static void url_decode_inplace(char *str)
{
    char *src = str;
    char *dst = str;
    char a, b;
    
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
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

/* External functions from uv_server.c for header access */
extern const char* uv_http_get_header(void *conn, const char *name);
extern int uv_http_iterate_headers_with_prefix(void *conn_ptr, const char *prefix,
                                                void (*callback)(const char *name, const char *value, void *user_data),
                                                void *user_data);

/* External auth functions from s3_auth.c */
extern int buckets_s3_parse_auth_header(const char *auth_header, buckets_s3_request_t *req,
                                         char *date_out, size_t date_len,
                                         char *region_out, size_t region_len);

/**
 * Get header value from HTTP request (case-insensitive)
 */
static const char* get_header(buckets_http_request_t *http_req, const char *name)
{
    if (!http_req || !http_req->internal || !name) {
        return NULL;
    }
    
    /* Use the UV server's header getter */
    return uv_http_get_header(http_req->internal, name);
}

/**
 * Callback context for user metadata parsing
 */
typedef struct {
    buckets_s3_request_t *req;
} user_meta_ctx_t;

/**
 * Callback for each x-amz-meta-* header
 */
static void user_meta_callback(const char *name, const char *value, void *user_data)
{
    user_meta_ctx_t *ctx = (user_meta_ctx_t *)user_data;
    buckets_s3_request_t *req = ctx->req;
    
    if (req->user_meta_count >= BUCKETS_S3_MAX_USER_METADATA) {
        buckets_warn("Too many user metadata headers, ignoring: %s", name);
        return;
    }
    
    /* Strip "x-amz-meta-" prefix (11 characters) */
    const char *key = name + 11;
    
    /* Store key and value (will be freed when request is freed) */
    req->user_meta_keys[req->user_meta_count] = buckets_strdup(key);
    req->user_meta_values[req->user_meta_count] = buckets_strdup(value);
    req->user_meta_count++;
    
    buckets_debug("Parsed user metadata: %s = %s", key, value);
}

/**
 * Parse x-amz-meta-* headers from HTTP request
 * Returns number of user metadata entries found
 */
static int parse_user_metadata(buckets_http_request_t *http_req, buckets_s3_request_t *req)
{
    if (!http_req || !http_req->internal || !req) {
        return 0;
    }
    
    /* Get Content-Type header */
    const char *ct = get_header(http_req, "Content-Type");
    if (ct && ct[0] != '\0') {
        strncpy(req->content_type, ct, sizeof(req->content_type) - 1);
    }
    
    /* Iterate all x-amz-meta-* headers */
    user_meta_ctx_t ctx = { .req = req };
    uv_http_iterate_headers_with_prefix(http_req->internal, "x-amz-meta-",
                                        user_meta_callback, &ctx);
    
    if (req->user_meta_count > 0) {
        buckets_info("Parsed %d user metadata headers", req->user_meta_count);
    }
    
    return req->user_meta_count;
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
    
    /* Extract headers and user metadata */
    parse_user_metadata(http_req, req);
    
    req->content_length = http_req->body_len;
    
    /* Parse x-amz-date header */
    const char *amz_date = get_header(http_req, "x-amz-date");
    if (amz_date && amz_date[0] != '\0') {
        strncpy(req->date, amz_date, sizeof(req->date) - 1);
    } else {
        /* Fall back to Date header */
        const char *date_hdr = get_header(http_req, "Date");
        if (date_hdr && date_hdr[0] != '\0') {
            strncpy(req->date, date_hdr, sizeof(req->date) - 1);
        }
    }
    
    /* Parse Authorization header (AWS Signature V4) */
    const char *auth_hdr = get_header(http_req, "Authorization");
    if (auth_hdr && auth_hdr[0] != '\0') {
        char date_from_auth[32] = {0};
        char region_from_auth[64] = {0};
        
        if (buckets_s3_parse_auth_header(auth_hdr, req, 
                                          date_from_auth, sizeof(date_from_auth),
                                          region_from_auth, sizeof(region_from_auth)) == BUCKETS_OK) {
            /* Store region from credential scope for signature verification */
            strncpy(req->region, region_from_auth, sizeof(req->region) - 1);
            req->region[sizeof(req->region) - 1] = '\0';
            buckets_debug("Parsed Authorization: access_key=%s, region=%s", req->access_key, req->region);
        } else {
            buckets_debug("Failed to parse Authorization header");
        }
    }
    
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
    
    /* Free user metadata */
    for (int i = 0; i < req->user_meta_count; i++) {
        buckets_free(req->user_meta_keys[i]);
        buckets_free(req->user_meta_values[i]);
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
    
    /* Free user metadata */
    for (int i = 0; i < res->user_meta_count; i++) {
        buckets_free(res->user_meta_keys[i]);
        buckets_free(res->user_meta_values[i]);
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
    
    /* Check for health endpoint */
    if (req->uri && strcmp(req->uri, "/health") == 0) {
        res->status_code = 200;
        /* Note: Don't set body to string literal - it would be freed in s3_streaming.c.
         * A 200 with no body is sufficient for health checks. */
        res->body = NULL;
        res->body_len = 0;
        return;
    }
    
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
    
    /* Verify AWS Signature V4 authentication */
    if (buckets_s3_auth_enabled()) {
        ret = buckets_s3_verify_signature(s3_req, NULL);
        if (ret != BUCKETS_OK) {
            buckets_s3_request_free(s3_req);
            res->status_code = 403;
            buckets_http_response_set_header(res, "Content-Type", "application/xml");
            /* HEAD requests must not have a body per HTTP spec */
            if (strcmp(req->method, "HEAD") == 0) {
                res->body = NULL;
                res->body_len = 0;
            } else if (ret == BUCKETS_ERR_ACCESS_DENIED) {
                const char *xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<Error><Code>AccessDenied</Code>"
                    "<Message>Access Denied</Message></Error>";
                res->body = buckets_strdup(xml);
                res->body_len = strlen(xml);
            } else {
                const char *xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<Error><Code>SignatureDoesNotMatch</Code>"
                    "<Message>The request signature we calculated does not match the signature you provided</Message></Error>";
                res->body = buckets_strdup(xml);
                res->body_len = strlen(xml);
            }
            return;
        }
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
            /* Check for bucket versioning configuration */
            if (buckets_s3_is_versioning_request(s3_req)) {
                /* PUT /{bucket}?versioning - Set bucket versioning */
                ret = buckets_s3_put_bucket_versioning(s3_req, s3_res);
            } else {
                /* PUT bucket (create bucket) */
                ret = buckets_s3_put_bucket(s3_req, s3_res);
            }
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
            } else if (buckets_s3_has_version_id(s3_req)) {
                /* GET /{bucket}/{key}?versionId={id} - Get specific version */
                ret = buckets_s3_get_object_version(s3_req, s3_res);
            } else {
                /* GET object */
                ret = buckets_s3_get_object(s3_req, s3_res);
            }
        } else if (s3_req->bucket[0] != '\0') {
            /* Check for GetBucketLocation request */
            if (has_query_param(s3_req, "location")) {
                /* GET /{bucket}?location - Get bucket location */
                /* Return us-east-1 as the default region */
                const char *location_xml = 
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<LocationConstraint xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"/>";
                s3_res->body = buckets_strdup(location_xml);
                s3_res->body_len = strlen(location_xml);
                s3_res->status_code = 200;
                ret = BUCKETS_OK;
            } else if (buckets_s3_is_list_versions_request(s3_req)) {
                /* GET /{bucket}?versions - List object versions */
                ret = buckets_s3_list_object_versions(s3_req, s3_res);
            } else if (buckets_s3_is_versioning_request(s3_req)) {
                /* GET /{bucket}?versioning - Get bucket versioning status */
                ret = buckets_s3_get_bucket_versioning(s3_req, s3_res);
            } else {
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
    
    /* HEAD requests must not have a body per HTTP spec */
    if (strcmp(method, "HEAD") == 0) {
        /* Don't send body for HEAD, but preserve Content-Length for object metadata */
        if (s3_res->body) {
            buckets_free(s3_res->body);
            s3_res->body = NULL;
        }
        res->body = NULL;
        res->body_len = 0;
    } else if (s3_res->body) {
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
    
    /* Set version ID header if present */
    if (s3_res->version_id[0] != '\0') {
        buckets_http_response_set_header(res, "x-amz-version-id", s3_res->version_id);
        buckets_debug("Setting response header: x-amz-version-id: %s", s3_res->version_id);
    }
    
    /* Set user metadata headers (x-amz-meta-*) */
    for (int i = 0; i < s3_res->user_meta_count; i++) {
        char header_name[256];
        snprintf(header_name, sizeof(header_name), "x-amz-meta-%s", s3_res->user_meta_keys[i]);
        buckets_http_response_set_header(res, header_name, s3_res->user_meta_values[i]);
        buckets_debug("Setting response header: %s: %s", header_name, s3_res->user_meta_values[i]);
    }
    
    /* For HEAD requests, set body_len from s3_res->content_length 
     * so the HTTP layer uses it for Content-Length header.
     * We don't send body for HEAD, but Content-Length should be object size. */
    if (strcmp(method, "HEAD") == 0) {
        buckets_debug("HEAD response: content_length=%lld, body_len=%zu", 
                      (long long)s3_res->content_length, s3_res->body_len);
        if (s3_res->content_length > 0) {
            res->body_len = (size_t)s3_res->content_length;
            buckets_debug("HEAD: Set res->body_len to %zu for Content-Length", res->body_len);
        } else if (s3_res->body_len > 0) {
            /* Fallback: use body_len if content_length wasn't set */
            res->body_len = s3_res->body_len;
            buckets_debug("HEAD: Fallback to body_len=%zu for Content-Length", res->body_len);
        }
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
