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

/* Maximum user metadata entries per object */
#define BUCKETS_S3_MAX_USER_METADATA 32

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
    
    /* User metadata (x-amz-meta-*) */
    char *user_meta_keys[BUCKETS_S3_MAX_USER_METADATA];    /* Keys without x-amz-meta- prefix */
    char *user_meta_values[BUCKETS_S3_MAX_USER_METADATA];  /* Values */
    int user_meta_count;       /* Number of user metadata entries */
    
    /* Authentication */
    char access_key[128];
    char signature[512];
    char signed_headers[512];
    char date[64];             /* x-amz-date */
    char region[64];           /* Region from credential scope */
    
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
    
    /* User metadata (x-amz-meta-*) - for GET/HEAD responses */
    char *user_meta_keys[BUCKETS_S3_MAX_USER_METADATA];    /* Keys without prefix */
    char *user_meta_values[BUCKETS_S3_MAX_USER_METADATA];  /* Values */
    int user_meta_count;       /* Number of user metadata entries */
    
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
 * Get secret key for access key (legacy - use buckets_credentials_get_secret)
 * 
 * Looks up the secret key for the given access key.
 * 
 * @param access_key Access key ID
 * @param secret_key Output: secret key buffer (128 bytes)
 * @return BUCKETS_OK if found
 */
int buckets_s3_get_secret_key(const char *access_key, char *secret_key);

/**
 * Initialize authentication system
 * 
 * @param enabled Enable authentication checking
 * @return BUCKETS_OK on success
 */
int buckets_s3_auth_init(bool enabled);

/**
 * Check if authentication is enabled
 * 
 * @return true if authentication is enabled
 */
bool buckets_s3_auth_enabled(void);

/**
 * Enable/disable authentication
 * 
 * @param enabled Enable or disable authentication
 */
void buckets_s3_auth_set_enabled(bool enabled);

/**
 * Parse Authorization header
 * 
 * Extracts access key, signature, and other fields from Authorization header.
 * 
 * @param auth_header Authorization header value
 * @param req S3 request to populate
 * @param date_out Output: date from credential scope
 * @param date_len Size of date_out buffer
 * @param region_out Output: region from credential scope
 * @param region_len Size of region_out buffer
 * @return BUCKETS_OK on success
 */
int buckets_s3_parse_auth_header(const char *auth_header, buckets_s3_request_t *req,
                                  char *date_out, size_t date_len,
                                  char *region_out, size_t region_len);

/**
 * Check if request has authentication credentials
 * 
 * @param req S3 request
 * @return true if request has access key and signature
 */
bool buckets_s3_has_auth(buckets_s3_request_t *req);

/* ===================================================================
 * Credential Management
 * ===================================================================*/

/**
 * Initialize credential system
 * 
 * Loads credentials from disk or creates defaults.
 * 
 * @param data_dir Data directory for credential storage
 * @return BUCKETS_OK on success
 */
int buckets_credentials_init(const char *data_dir);

/**
 * Cleanup credential system
 */
void buckets_credentials_cleanup(void);

/**
 * Get secret key for access key
 * 
 * @param access_key Access key ID
 * @param secret_key Output: secret key buffer
 * @param secret_len Size of secret_key buffer
 * @return BUCKETS_OK if found
 */
int buckets_credentials_get_secret(const char *access_key, char *secret_key, size_t secret_len);

/**
 * Validate access key exists and is enabled
 * 
 * @param access_key Access key ID
 * @return BUCKETS_OK if valid
 */
int buckets_credentials_validate(const char *access_key);

/**
 * Update last_used timestamp for access key
 * 
 * @param access_key Access key ID
 */
void buckets_credentials_touch(const char *access_key);

/**
 * Get policy for access key
 * 
 * @param access_key Access key ID
 * @param policy Output: policy buffer
 * @param policy_len Size of policy buffer
 * @return BUCKETS_OK if found
 */
int buckets_credentials_get_policy(const char *access_key, char *policy, size_t policy_len);

/**
 * Create new credential with generated keys
 * 
 * @param name Human-readable name (optional)
 * @param policy Policy: "readwrite", "readonly", "writeonly" (optional, default "readwrite")
 * @param access_key_out Output: generated access key (min 21 bytes)
 * @param access_key_len Size of access_key_out buffer
 * @param secret_key_out Output: generated secret key (min 41 bytes)
 * @param secret_key_len Size of secret_key_out buffer
 * @return BUCKETS_OK on success
 */
int buckets_credentials_create(const char *name, const char *policy,
                                char *access_key_out, size_t access_key_len,
                                char *secret_key_out, size_t secret_key_len);

/**
 * Delete credential by access key
 * 
 * @param access_key Access key ID to delete
 * @return BUCKETS_OK on success
 */
int buckets_credentials_delete(const char *access_key);

/**
 * Enable/disable credential
 * 
 * @param access_key Access key ID
 * @param enabled True to enable, false to disable
 * @return BUCKETS_OK on success
 */
int buckets_credentials_set_enabled(const char *access_key, bool enabled);

/**
 * List all credentials as JSON
 * 
 * Returns JSON with credential info (excluding secret keys).
 * Caller must free returned string.
 * 
 * @return JSON string or NULL on error
 */
char* buckets_credentials_list(void);

/**
 * Get credential count
 * 
 * @return Number of credentials
 */
int buckets_credentials_count(void);

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
 * Bucket Operations
 * ===================================================================*/

/**
 * PUT Bucket operation (CreateBucket)
 * 
 * Creates a new bucket.
 * 
 * @param req S3 request (bucket name in req->bucket)
 * @param res Output: S3 response
 * @return BUCKETS_OK on success
 */
int buckets_s3_put_bucket(buckets_s3_request_t *req,
                          buckets_s3_response_t *res);

/**
 * DELETE Bucket operation
 * 
 * Deletes an empty bucket. Returns 409 Conflict if bucket is not empty.
 * 
 * @param req S3 request (bucket name in req->bucket)
 * @param res Output: S3 response
 * @return BUCKETS_OK on success
 */
int buckets_s3_delete_bucket(buckets_s3_request_t *req,
                             buckets_s3_response_t *res);

/**
 * HEAD Bucket operation
 * 
 * Checks if a bucket exists and is accessible.
 * Returns 200 if exists, 404 if not found.
 * 
 * @param req S3 request (bucket name in req->bucket)
 * @param res Output: S3 response (no body)
 * @return BUCKETS_OK on success
 */
int buckets_s3_head_bucket(buckets_s3_request_t *req,
                           buckets_s3_response_t *res);

/**
 * LIST Buckets operation
 * 
 * Lists all buckets owned by the authenticated user.
 * Returns XML with bucket names and creation dates.
 * 
 * @param req S3 request (no bucket/key needed)
 * @param res Output: S3 response with XML body
 * @return BUCKETS_OK on success
 */
int buckets_s3_list_buckets(buckets_s3_request_t *req,
                            buckets_s3_response_t *res);

/**
 * LIST Objects v1 operation
 * 
 * Lists objects in a bucket (S3 v1 API).
 * Supports: prefix, marker, max-keys, delimiter
 * 
 * Query parameters:
 * - prefix: Filter by key prefix
 * - marker: Pagination marker (start after this key)
 * - max-keys: Maximum number of keys to return (default 1000)
 * - delimiter: Group keys by common prefix (for "folders")
 * 
 * @param req S3 request (bucket name required)
 * @param res Output: S3 response with XML body (ListBucketResult)
 * @return BUCKETS_OK on success
 */
int buckets_s3_list_objects_v1(buckets_s3_request_t *req,
                               buckets_s3_response_t *res);

/**
 * LIST Objects v2 operation
 * 
 * Lists objects in a bucket (S3 v2 API - recommended).
 * Supports: prefix, continuation-token, max-keys, delimiter, start-after
 * 
 * Query parameters:
 * - prefix: Filter by key prefix
 * - continuation-token: Pagination token from previous response
 * - max-keys: Maximum number of keys to return (default 1000)
 * - delimiter: Group keys by common prefix (for "folders")
 * - start-after: Start listing after this key
 * - list-type=2: Required parameter to use v2 API
 * 
 * @param req S3 request (bucket name required)
 * @param res Output: S3 response with XML body (ListBucketResult)
 * @return BUCKETS_OK on success
 */
int buckets_s3_list_objects_v2(buckets_s3_request_t *req,
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

/* ===================================================================
 * Multipart Upload Operations (Weeks 39-40)
 * ===================================================================*/

/**
 * Multipart upload part information
 */
typedef struct {
    int part_number;           /* Part number (1-10000) */
    char etag[64];             /* MD5 hash of part */
    size_t size;               /* Part size in bytes */
    time_t uploaded;           /* Upload timestamp */
} buckets_s3_part_t;

/**
 * Multipart upload session
 */
typedef struct {
    char upload_id[64];        /* Upload ID (UUID) */
    char bucket[256];          /* Bucket name */
    char key[1024];            /* Object key */
    time_t initiated;          /* Initiation timestamp */
    int part_count;            /* Number of parts uploaded */
    buckets_s3_part_t *parts;  /* Array of parts */
} buckets_s3_upload_t;

/**
 * Initiate multipart upload
 * 
 * POST /{bucket}/{key}?uploads
 * 
 * Starts a new multipart upload session and returns an upload ID.
 * Client must use this upload ID for all subsequent part uploads
 * and the final complete/abort operations.
 * 
 * @param req S3 request (bucket and key required)
 * @param res Output: S3 response with XML body (InitiateMultipartUploadResult)
 * @return BUCKETS_OK on success
 */
int buckets_s3_initiate_multipart_upload(buckets_s3_request_t *req,
                                         buckets_s3_response_t *res);

/**
 * Upload part
 * 
 * PUT /{bucket}/{key}?uploadId={id}&partNumber={n}
 * 
 * Uploads a single part of a multipart upload. Parts can be uploaded
 * in any order and in parallel. Each part must be at least 5MB except
 * the last part. Part numbers must be between 1-10000.
 * 
 * @param req S3 request (bucket, key, uploadId, partNumber required)
 * @param res Output: S3 response with ETag header
 * @return BUCKETS_OK on success
 */
int buckets_s3_upload_part(buckets_s3_request_t *req,
                            buckets_s3_response_t *res);

/**
 * Complete multipart upload
 * 
 * POST /{bucket}/{key}?uploadId={id}
 * 
 * Completes a multipart upload by assembling the previously uploaded
 * parts. The request body contains an XML list of part numbers and ETags.
 * Parts are concatenated in ascending part number order to create the
 * final object.
 * 
 * @param req S3 request (bucket, key, uploadId required, body contains XML)
 * @param res Output: S3 response with XML body (CompleteMultipartUploadResult)
 * @return BUCKETS_OK on success
 */
int buckets_s3_complete_multipart_upload(buckets_s3_request_t *req,
                                         buckets_s3_response_t *res);

/**
 * Abort multipart upload
 * 
 * DELETE /{bucket}/{key}?uploadId={id}
 * 
 * Aborts a multipart upload and deletes all uploaded parts.
 * After aborting, the upload ID is invalid and cannot be reused.
 * 
 * @param req S3 request (bucket, key, uploadId required)
 * @param res Output: S3 response (204 No Content)
 * @return BUCKETS_OK on success
 */
int buckets_s3_abort_multipart_upload(buckets_s3_request_t *req,
                                      buckets_s3_response_t *res);

/**
 * List parts
 * 
 * GET /{bucket}/{key}?uploadId={id}
 * 
 * Lists the parts that have been uploaded for a specific multipart upload.
 * Supports pagination with max-parts and part-number-marker parameters.
 * 
 * @param req S3 request (bucket, key, uploadId required)
 * @param res Output: S3 response with XML body (ListPartsResult)
 * @return BUCKETS_OK on success
 */
int buckets_s3_list_parts(buckets_s3_request_t *req,
                          buckets_s3_response_t *res);

/* ===================================================================
 * Versioning Operations (Week 41)
 * ===================================================================*/

/**
 * PUT bucket versioning
 * 
 * PUT /{bucket}?versioning
 * 
 * Enables or suspends versioning for a bucket. Once versioning is enabled,
 * it cannot be disabled - only suspended. When enabled, every PUT creates
 * a new version; when suspended, PUTs still create versions but with "null"
 * version ID.
 * 
 * @param req S3 request (bucket, body contains VersioningConfiguration XML)
 * @param res Output: S3 response
 * @return BUCKETS_OK on success
 */
int buckets_s3_put_bucket_versioning(buckets_s3_request_t *req,
                                      buckets_s3_response_t *res);

/**
 * GET bucket versioning
 * 
 * GET /{bucket}?versioning
 * 
 * Returns the versioning status of a bucket.
 * 
 * @param req S3 request (bucket)
 * @param res Output: S3 response with VersioningConfiguration XML
 * @return BUCKETS_OK on success
 */
int buckets_s3_get_bucket_versioning(buckets_s3_request_t *req,
                                      buckets_s3_response_t *res);

/**
 * GET object version
 * 
 * GET /{bucket}/{key}?versionId={id}
 * 
 * Retrieves a specific version of an object. If the version is a
 * delete marker, returns 404 with x-amz-delete-marker header.
 * 
 * @param req S3 request (bucket, key, versionId in query params)
 * @param res Output: S3 response
 * @return BUCKETS_OK on success
 */
int buckets_s3_get_object_version(buckets_s3_request_t *req,
                                   buckets_s3_response_t *res);

/**
 * DELETE object versioned
 * 
 * DELETE /{bucket}/{key}?versionId={id}  - Hard delete specific version
 * DELETE /{bucket}/{key}                  - Create delete marker (if versioned)
 * 
 * With versionId: permanently deletes that specific version.
 * Without versionId (and versioning enabled): creates a delete marker,
 * making the object appear deleted while preserving all versions.
 * 
 * @param req S3 request (bucket, key, optional versionId)
 * @param res Output: S3 response with version ID
 * @return BUCKETS_OK on success
 */
int buckets_s3_delete_object_versioned(buckets_s3_request_t *req,
                                        buckets_s3_response_t *res);

/**
 * LIST object versions
 * 
 * GET /{bucket}?versions
 * 
 * Lists all versions and delete markers for objects in a bucket.
 * Supports prefix, delimiter, key-marker, version-id-marker, and max-keys.
 * 
 * @param req S3 request (bucket, query params for filtering)
 * @param res Output: S3 response with ListVersionsResult XML
 * @return BUCKETS_OK on success
 */
int buckets_s3_list_object_versions(buckets_s3_request_t *req,
                                     buckets_s3_response_t *res);

/**
 * Check if request has versionId parameter
 */
bool buckets_s3_has_version_id(buckets_s3_request_t *req);

/**
 * Check if request is for bucket versioning configuration
 */
bool buckets_s3_is_versioning_request(buckets_s3_request_t *req);

/**
 * Check if request is for list versions
 */
bool buckets_s3_is_list_versions_request(buckets_s3_request_t *req);

/**
 * Get/set bucket versioning status
 */
int buckets_get_bucket_versioning(const char *bucket, bool *enabled, bool *suspended);
int buckets_set_bucket_versioning(const char *bucket, bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_S3_H */
