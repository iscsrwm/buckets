/**
 * S3 API Layer
 * 
 * Amazon S3-compatible REST API for object storage operations.
 * 
 * Phase 9: S3 API (Weeks 35-42)
 */

#ifndef BUCKETS_S3_H
#define BUCKETS_S3_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "buckets.h"
#include "buckets_net.h"

/* ===================================================================
 * S3 Request/Response Types
 * ===================================================================*/

/**
 * S3 request structure
 */
typedef struct {
    char bucket[256];          /* Bucket name */
    char key[1024];            /* Object key (path) */
    char version_id[64];       /* Version ID (optional) */
    
    /* Request body */
    const char *body;          /* Request body (not owned) */
    size_t body_len;           /* Body length */
    
    /* Headers */
    char content_type[128];
    char content_md5[64];
    i64 content_length;
    char if_match[64];         /* ETag for conditional requests */
    char if_none_match[64];    /* ETag for conditional requests */
    
    /* Authentication */
    char access_key[128];
    char signature[512];
    char signed_headers[512];
    char date[64];             /* x-amz-date */
    
    /* Query parameters */
    char **query_params_keys;
    char **query_params_values;
    int query_count;
    
    /* Internal */
    buckets_http_request_t *http_req;  /* Underlying HTTP request */
} buckets_s3_request_t;

/**
 * S3 response structure
 */
typedef struct {
    int status_code;           /* HTTP status (200, 404, etc.) */
    char *body;                /* Response body (XML or binary) */
    size_t body_len;           /* Body length */
    
    /* Headers */
    char etag[64];             /* Object ETag */
    i64 content_length;
    char content_type[128];
    char last_modified[64];    /* RFC 2822 format */
    char version_id[64];       /* Version ID */
    
    /* Error info */
    char error_code[64];       /* S3 error code */
    char error_message[256];   /* Error message */
} buckets_s3_response_t;

/**
 * S3 error codes
 */
typedef enum {
    S3_ERR_NONE = 0,
    S3_ERR_NO_SUCH_BUCKET,
    S3_ERR_NO_SUCH_KEY,
    S3_ERR_BUCKET_ALREADY_EXISTS,
    S3_ERR_INVALID_BUCKET_NAME,
    S3_ERR_INVALID_KEY,
    S3_ERR_ACCESS_DENIED,
    S3_ERR_SIGNATURE_MISMATCH,
    S3_ERR_ENTITY_TOO_LARGE,
    S3_ERR_INTERNAL_ERROR,
    S3_ERR_INVALID_REQUEST
} buckets_s3_error_t;

/* ===================================================================
 * S3 Handler Functions
 * ===================================================================*/

/**
 * Initialize S3 API layer
 * 
 * Sets up S3 request handlers on the HTTP server.
 * 
 * @param server HTTP server from Phase 8
 * @return BUCKETS_OK on success
 */
int buckets_s3_init(buckets_http_server_t *server);

/**
 * S3 request handler
 * 
 * Main entry point for S3 requests. Parses request, authenticates,
 * and routes to appropriate operation handler.
 * 
 * @param req HTTP request
 * @param res HTTP response
 * @param user_data User data
 */
void buckets_s3_handler(buckets_http_request_t *req,
                        buckets_http_response_t *res,
                        void *user_data);

/**
 * Parse S3 request from HTTP request
 * 
 * @param http_req HTTP request
 * @param s3_req Output: S3 request (caller must free with buckets_s3_request_free)
 * @return BUCKETS_OK on success
 */
int buckets_s3_parse_request(buckets_http_request_t *http_req,
                              buckets_s3_request_t **s3_req);

/**
 * Free S3 request
 * 
 * @param req S3 request
 */
void buckets_s3_request_free(buckets_s3_request_t *req);

/**
 * Free S3 response
 * 
 * @param res S3 response
 */
void buckets_s3_response_free(buckets_s3_response_t *res);

/* ===================================================================
 * S3 Authentication (AWS Signature V4)
 * ===================================================================*/

/**
 * Verify AWS Signature V4
 * 
 * Verifies the Authorization header or query string signature.
 * 
 * @param req S3 request with signature
 * @param secret_key Secret access key for verification
 * @return BUCKETS_OK if signature is valid
 */
int buckets_s3_verify_signature(buckets_s3_request_t *req,
                                 const char *secret_key);

/**
 * Get secret key for access key
 * 
 * Looks up the secret key for the given access key.
 * In production, this would query a key store.
 * 
 * @param access_key Access key ID
 * @param secret_key Output: secret key buffer (128 bytes)
 * @return BUCKETS_OK if found
 */
int buckets_s3_get_secret_key(const char *access_key, char *secret_key);

/* ===================================================================
 * S3 Object Operations
 * ===================================================================*/

/**
 * PUT Object operation
 * 
 * Writes an object to storage.
 * 
 * @param req S3 request
 * @param res Output: S3 response
 * @return BUCKETS_OK on success
 */
int buckets_s3_put_object(buckets_s3_request_t *req,
                          buckets_s3_response_t *res);

/**
 * GET Object operation
 * 
 * Reads an object from storage.
 * 
 * @param req S3 request
 * @param res Output: S3 response
 * @return BUCKETS_OK on success
 */
int buckets_s3_get_object(buckets_s3_request_t *req,
                          buckets_s3_response_t *res);

/**
 * DELETE Object operation
 * 
 * Deletes an object from storage.
 * 
 * @param req S3 request
 * @param res Output: S3 response
 * @return BUCKETS_OK on success
 */
int buckets_s3_delete_object(buckets_s3_request_t *req,
                             buckets_s3_response_t *res);

/**
 * HEAD Object operation
 * 
 * Returns object metadata without body.
 * 
 * @param req S3 request
 * @param res Output: S3 response (no body)
 * @return BUCKETS_OK on success
 */
int buckets_s3_head_object(buckets_s3_request_t *req,
                           buckets_s3_response_t *res);

/* ===================================================================
 * XML Response Generation
 * ===================================================================*/

/**
 * Generate XML success response
 * 
 * @param res S3 response to populate
 * @param root_element Root XML element (e.g., "PutObjectResult")
 * @return BUCKETS_OK on success
 */
int buckets_s3_xml_success(buckets_s3_response_t *res,
                           const char *root_element);

/**
 * Generate XML error response
 * 
 * @param res S3 response to populate
 * @param error_code S3 error code
 * @param message Error message
 * @param resource Resource path
 * @return BUCKETS_OK on success
 */
int buckets_s3_xml_error(buckets_s3_response_t *res,
                         const char *error_code,
                         const char *message,
                         const char *resource);

/**
 * Add XML element to response
 * 
 * Helper to add key-value elements to XML response.
 * 
 * @param xml_body XML body buffer
 * @param max_len Maximum buffer length
 * @param key Element name
 * @param value Element value
 * @return BUCKETS_OK on success
 */
int buckets_s3_xml_add_element(char *xml_body,
                               size_t max_len,
                               const char *key,
                               const char *value);

/* ===================================================================
 * Utility Functions
 * ===================================================================*/

/**
 * Calculate ETag for object
 * 
 * Calculates MD5 hash of object data for ETag header.
 * 
 * @param data Object data
 * @param len Data length
 * @param etag Output: ETag string (64 bytes)
 * @return BUCKETS_OK on success
 */
int buckets_s3_calculate_etag(const void *data, size_t len, char *etag);

/**
 * Format timestamp for Last-Modified header
 * 
 * Formats time_t as RFC 2822 date string.
 * 
 * @param timestamp Unix timestamp
 * @param buffer Output buffer (64 bytes)
 * @return BUCKETS_OK on success
 */
int buckets_s3_format_timestamp(time_t timestamp, char *buffer);

/**
 * Validate bucket name
 * 
 * Checks if bucket name follows S3 naming rules.
 * 
 * @param bucket Bucket name
 * @return true if valid
 */
bool buckets_s3_validate_bucket_name(const char *bucket);

/**
 * Validate object key
 * 
 * Checks if object key is valid.
 * 
 * @param key Object key
 * @return true if valid
 */
bool buckets_s3_validate_object_key(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_S3_H */
