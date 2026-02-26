/**
 * S3 Object Operations
 * 
 * Implements PUT/GET/DELETE/HEAD object operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <openssl/evp.h>

#include "buckets.h"
#include "buckets_s3.h"

#define MD5_DIGEST_LENGTH 16

/* ===================================================================
 * Utility Functions
 * ===================================================================*/

int buckets_s3_calculate_etag(const void *data, size_t len, char *etag)
{
    if (!data || !etag) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Use EVP API for MD5 (non-deprecated) */
    unsigned char md5[MD5_DIGEST_LENGTH];
    unsigned int md5_len = 0;
    
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return BUCKETS_ERR_CRYPTO;
    }
    
    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, md5, &md5_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return BUCKETS_ERR_CRYPTO;
    }
    
    EVP_MD_CTX_free(ctx);
    
    /* Convert to hex string */
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(etag + (i * 2), "%02x", md5[i]);
    }
    etag[MD5_DIGEST_LENGTH * 2] = '\0';
    
    return BUCKETS_OK;
}

int buckets_s3_format_timestamp(time_t timestamp, char *buffer)
{
    if (!buffer) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    struct tm *tm_info = gmtime(&timestamp);
    if (!tm_info) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Format as RFC 2822: "Day, DD Mon YYYY HH:MM:SS GMT" */
    strftime(buffer, 64, "%a, %d %b %Y %H:%M:%S GMT", tm_info);
    
    return BUCKETS_OK;
}

bool buckets_s3_validate_bucket_name(const char *bucket)
{
    if (!bucket || bucket[0] == '\0') {
        return false;
    }
    
    size_t len = strlen(bucket);
    
    /* Bucket name length: 3-63 characters */
    if (len < 3 || len > 63) {
        return false;
    }
    
    /* Must start with lowercase letter or number */
    if (!islower(bucket[0]) && !isdigit(bucket[0])) {
        return false;
    }
    
    /* Can contain lowercase letters, numbers, dots, and hyphens */
    for (size_t i = 0; i < len; i++) {
        char c = bucket[i];
        if (!islower(c) && !isdigit(c) && c != '.' && c != '-') {
            return false;
        }
    }
    
    /* Cannot end with dash */
    if (bucket[len - 1] == '-') {
        return false;
    }
    
    /* Cannot have consecutive dots */
    if (strstr(bucket, "..")) {
        return false;
    }
    
    return true;
}

bool buckets_s3_validate_object_key(const char *key)
{
    if (!key || key[0] == '\0') {
        return false;
    }
    
    size_t len = strlen(key);
    
    /* Key length: 1-1024 characters */
    if (len < 1 || len > 1024) {
        return false;
    }
    
    /* Key cannot start with slash */
    if (key[0] == '/') {
        return false;
    }
    
    /* All printable ASCII characters are valid */
    /* We're being permissive here */
    return true;
}

/* ===================================================================
 * Object Operations
 * ===================================================================*/

int buckets_s3_put_object(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Validate bucket name */
    if (!buckets_s3_validate_bucket_name(req->bucket)) {
        buckets_s3_xml_error(res, "InvalidBucketName",
                            "The specified bucket name is not valid",
                            req->bucket);
        return BUCKETS_OK;  /* Error response generated */
    }
    
    /* Validate object key */
    if (!buckets_s3_validate_object_key(req->key)) {
        buckets_s3_xml_error(res, "InvalidKey",
                            "The specified key is not valid",
                            req->key);
        return BUCKETS_OK;
    }
    
    /* For Week 35, we'll store in a simple file-based structure */
    /* In production, this would use buckets_object_write() */
    
    /* Create bucket directory if needed */
    char bucket_path[2048];
    snprintf(bucket_path, sizeof(bucket_path), "/tmp/buckets-data/%s", req->bucket);
    
    char cmd[2560];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", bucket_path);
    if (system(cmd) != 0) {
        buckets_s3_xml_error(res, "InternalError",
                            "Failed to create bucket directory",
                            req->bucket);
        return BUCKETS_OK;
    }
    
    /* Write object to file */
    char object_path[3072];
    snprintf(object_path, sizeof(object_path), "%s/%s", bucket_path, req->key);
    
    FILE *fp = fopen(object_path, "wb");
    if (!fp) {
        buckets_s3_xml_error(res, "InternalError",
                            "Failed to write object",
                            req->key);
        return BUCKETS_OK;
    }
    
    if (req->body && req->body_len > 0) {
        size_t written = fwrite(req->body, 1, req->body_len, fp);
        if (written != req->body_len) {
            fclose(fp);
            buckets_s3_xml_error(res, "InternalError",
                                "Failed to write object data",
                                req->key);
            return BUCKETS_OK;
        }
    }
    
    fclose(fp);
    
    /* Calculate ETag */
    if (req->body && req->body_len > 0) {
        buckets_s3_calculate_etag(req->body, req->body_len, res->etag);
    } else {
        /* Empty object */
        buckets_s3_calculate_etag("", 0, res->etag);
    }
    
    /* Generate success response */
    buckets_s3_xml_success(res, "PutObjectResult");
    
    buckets_debug("PUT object: %s/%s (%zu bytes, ETag: %s)",
                  req->bucket, req->key, req->body_len, res->etag);
    
    return BUCKETS_OK;
}

int buckets_s3_get_object(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Validate bucket and key */
    if (!buckets_s3_validate_bucket_name(req->bucket)) {
        buckets_s3_xml_error(res, "InvalidBucketName",
                            "The specified bucket name is not valid",
                            req->bucket);
        return BUCKETS_OK;
    }
    
    if (!buckets_s3_validate_object_key(req->key)) {
        buckets_s3_xml_error(res, "InvalidKey",
                            "The specified key is not valid",
                            req->key);
        return BUCKETS_OK;
    }
    
    /* Read object from file */
    char object_path[3072];
    snprintf(object_path, sizeof(object_path), "/tmp/buckets-data/%s/%s",
             req->bucket, req->key);
    
    FILE *fp = fopen(object_path, "rb");
    if (!fp) {
        /* Object not found */
        buckets_s3_xml_error(res, "NoSuchKey",
                            "The specified key does not exist",
                            req->key);
        return BUCKETS_OK;
    }
    
    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size < 0) {
        fclose(fp);
        buckets_s3_xml_error(res, "InternalError",
                            "Failed to read object",
                            req->key);
        return BUCKETS_OK;
    }
    
    /* Allocate buffer for object data */
    char *object_data = buckets_malloc(file_size + 1);
    if (!object_data) {
        fclose(fp);
        buckets_s3_xml_error(res, "InternalError",
                            "Out of memory",
                            req->key);
        return BUCKETS_OK;
    }
    
    /* Read object data */
    size_t bytes_read = fread(object_data, 1, file_size, fp);
    fclose(fp);
    
    if (bytes_read != (size_t)file_size) {
        buckets_free(object_data);
        buckets_s3_xml_error(res, "InternalError",
                            "Failed to read object data",
                            req->key);
        return BUCKETS_OK;
    }
    
    object_data[file_size] = '\0';
    
    /* Calculate ETag */
    buckets_s3_calculate_etag(object_data, file_size, res->etag);
    
    /* Set response */
    res->status_code = 200;
    res->body = object_data;
    res->body_len = file_size;
    res->content_length = file_size;
    
    /* Set content type (default to application/octet-stream) */
    if (req->content_type[0] != '\0') {
        strncpy(res->content_type, req->content_type, sizeof(res->content_type) - 1);
        res->content_type[sizeof(res->content_type) - 1] = '\0';
    } else {
        strcpy(res->content_type, "application/octet-stream");
    }
    
    /* Format last modified time */
    buckets_s3_format_timestamp(time(NULL), res->last_modified);
    
    buckets_debug("GET object: %s/%s (%zu bytes, ETag: %s)",
                  req->bucket, req->key, res->body_len, res->etag);
    
    return BUCKETS_OK;
}

int buckets_s3_delete_object(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Validate inputs */
    if (!buckets_s3_validate_bucket_name(req->bucket) ||
        !buckets_s3_validate_object_key(req->key)) {
        buckets_s3_xml_error(res, "InvalidRequest",
                            "Invalid bucket or key",
                            req->key);
        return BUCKETS_OK;
    }
    
    /* Delete object file */
    char object_path[3072];
    snprintf(object_path, sizeof(object_path), "/tmp/buckets-data/%s/%s",
             req->bucket, req->key);
    
    if (remove(object_path) != 0) {
        /* Object might not exist - S3 DELETE is idempotent */
        buckets_debug("DELETE object not found: %s/%s", req->bucket, req->key);
    } else {
        buckets_debug("DELETE object: %s/%s", req->bucket, req->key);
    }
    
    /* DELETE always returns 204 No Content (success) */
    res->status_code = 204;
    res->body = NULL;
    res->body_len = 0;
    
    return BUCKETS_OK;
}

int buckets_s3_head_object(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* HEAD is like GET but returns no body */
    int ret = buckets_s3_get_object(req, res);
    if (ret != BUCKETS_OK) {
        return ret;
    }
    
    /* Free body if allocated */
    if (res->body) {
        buckets_free(res->body);
        res->body = NULL;
        res->body_len = 0;
    }
    
    /* Change status to 200 if it was set by GET */
    if (res->status_code == 200) {
        buckets_debug("HEAD object: %s/%s (ETag: %s)",
                      req->bucket, req->key, res->etag);
    }
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Bucket Operations
 * ===================================================================*/

int buckets_s3_put_bucket(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (req->bucket[0] == '\0') {
        return buckets_s3_xml_error(res, "InvalidBucketName",
                                     "Bucket name is required",
                                     "/");
    }
    
    /* Validate bucket name */
    if (!buckets_s3_validate_bucket_name(req->bucket)) {
        return buckets_s3_xml_error(res, "InvalidBucketName",
                                     "The specified bucket is not valid",
                                     req->bucket);
    }
    
    /* Create bucket directory */
    char bucket_path[2048];
    snprintf(bucket_path, sizeof(bucket_path), "/tmp/buckets-data/%s", req->bucket);
    
    /* Check if bucket already exists */
    struct stat st;
    if (stat(bucket_path, &st) == 0) {
        /* Bucket exists */
        if (S_ISDIR(st.st_mode)) {
            /* Bucket already exists - return 409 Conflict (BucketAlreadyOwnedByYou) */
            return buckets_s3_xml_error(res, "BucketAlreadyOwnedByYou",
                                         "Your previous request to create the named bucket succeeded and you already own it",
                                         req->bucket);
        }
    }
    
    /* Create bucket directory */
    if (mkdir(bucket_path, 0755) != 0) {
        buckets_error("Failed to create bucket directory: %s", bucket_path);
        return buckets_s3_xml_error(res, "InternalError",
                                     "Failed to create bucket",
                                     req->bucket);
    }
    
    buckets_info("Created bucket: %s", req->bucket);
    
    /* Return 200 OK with empty body */
    res->status_code = 200;
    res->body = NULL;
    res->body_len = 0;
    
    return BUCKETS_OK;
}

int buckets_s3_delete_bucket(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (req->bucket[0] == '\0') {
        return buckets_s3_xml_error(res, "InvalidBucketName",
                                     "Bucket name is required",
                                     "/");
    }
    
    /* Check if bucket exists */
    char bucket_path[2048];
    snprintf(bucket_path, sizeof(bucket_path), "/tmp/buckets-data/%s", req->bucket);
    
    struct stat st;
    if (stat(bucket_path, &st) != 0) {
        /* Bucket doesn't exist - return 404 */
        return buckets_s3_xml_error(res, "NoSuchBucket",
                                     "The specified bucket does not exist",
                                     req->bucket);
    }
    
    /* Check if bucket is empty by trying to read directory */
    DIR *dir = opendir(bucket_path);
    if (!dir) {
        return buckets_s3_xml_error(res, "InternalError",
                                     "Failed to access bucket",
                                     req->bucket);
    }
    
    struct dirent *entry;
    bool is_empty = true;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        is_empty = false;
        break;
    }
    closedir(dir);
    
    if (!is_empty) {
        /* Bucket not empty - return 409 Conflict */
        return buckets_s3_xml_error(res, "BucketNotEmpty",
                                     "The bucket you tried to delete is not empty",
                                     req->bucket);
    }
    
    /* Delete bucket directory */
    if (rmdir(bucket_path) != 0) {
        buckets_error("Failed to delete bucket: %s", bucket_path);
        return buckets_s3_xml_error(res, "InternalError",
                                     "Failed to delete bucket",
                                     req->bucket);
    }
    
    buckets_info("Deleted bucket: %s", req->bucket);
    
    /* Return 204 No Content */
    res->status_code = 204;
    res->body = NULL;
    res->body_len = 0;
    
    return BUCKETS_OK;
}

int buckets_s3_head_bucket(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (req->bucket[0] == '\0') {
        return buckets_s3_xml_error(res, "InvalidBucketName",
                                     "Bucket name is required",
                                     "/");
    }
    
    /* Check if bucket exists */
    char bucket_path[2048];
    snprintf(bucket_path, sizeof(bucket_path), "/tmp/buckets-data/%s", req->bucket);
    
    struct stat st;
    if (stat(bucket_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        /* Bucket doesn't exist - return 404 */
        return buckets_s3_xml_error(res, "NoSuchBucket",
                                     "The specified bucket does not exist",
                                     req->bucket);
    }
    
    buckets_debug("HEAD bucket: %s (exists)", req->bucket);
    
    /* Return 200 OK with no body */
    res->status_code = 200;
    res->body = NULL;
    res->body_len = 0;
    
    return BUCKETS_OK;
}

int buckets_s3_list_buckets(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Open buckets data directory */
    DIR *dir = opendir("/tmp/buckets-data");
    if (!dir) {
        /* No buckets exist yet - return empty list */
        buckets_debug("LIST buckets: directory doesn't exist, returning empty list");
        
        /* Build empty ListAllMyBucketsResult XML */
        char xml_body[4096];
        snprintf(xml_body, sizeof(xml_body),
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<ListAllMyBucketsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
                 "  <Owner>\n"
                 "    <ID>buckets-admin</ID>\n"
                 "    <DisplayName>buckets-admin</DisplayName>\n"
                 "  </Owner>\n"
                 "  <Buckets>\n"
                 "  </Buckets>\n"
                 "</ListAllMyBucketsResult>\n");
        
        res->status_code = 200;
        res->body = buckets_strdup(xml_body);
        res->body_len = strlen(xml_body);
        
        return BUCKETS_OK;
    }
    
    /* Build XML response with bucket list */
    char xml_body[16384];
    int offset = snprintf(xml_body, sizeof(xml_body),
                          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<ListAllMyBucketsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
                          "  <Owner>\n"
                          "    <ID>buckets-admin</ID>\n"
                          "    <DisplayName>buckets-admin</DisplayName>\n"
                          "  </Owner>\n"
                          "  <Buckets>\n");
    
    /* Iterate through buckets */
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Check if it's a directory */
        char bucket_path[2048];
        snprintf(bucket_path, sizeof(bucket_path), "/tmp/buckets-data/%s", entry->d_name);
        
        struct stat st;
        if (stat(bucket_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        
        /* Get bucket creation time */
        char timestamp[64];
        struct tm *tm_info = gmtime(&st.st_ctime);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
        
        /* Add bucket to XML */
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "    <Bucket>\n"
                           "      <Name>%s</Name>\n"
                           "      <CreationDate>%s</CreationDate>\n"
                           "    </Bucket>\n",
                           entry->d_name, timestamp);
        
        if (offset >= (int)sizeof(xml_body) - 200) {
            buckets_warn("LIST buckets: XML buffer nearly full");
            break;
        }
    }
    closedir(dir);
    
    /* Close XML */
    offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                       "  </Buckets>\n"
                       "</ListAllMyBucketsResult>\n");
    
    buckets_debug("LIST buckets: returning %d bytes of XML", offset);
    
    res->status_code = 200;
    res->body = buckets_strdup(xml_body);
    res->body_len = strlen(xml_body);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * LIST Objects Operations
 * ===================================================================*/

/**
 * Get query parameter value by key
 */
static const char* get_query_param(buckets_s3_request_t *req, const char *key)
{
    if (!req || !key) {
        return NULL;
    }
    
    for (int i = 0; i < req->query_count; i++) {
        if (req->query_params_keys[i] && 
            strcmp(req->query_params_keys[i], key) == 0) {
            return req->query_params_values[i];
        }
    }
    
    return NULL;
}

/**
 * Parse integer query parameter (returns default if not found or invalid)
 */
static int get_query_param_int(buckets_s3_request_t *req, const char *key, int default_value)
{
    const char *value = get_query_param(req, key);
    if (!value) {
        return default_value;
    }
    
    char *endptr;
    long result = strtol(value, &endptr, 10);
    
    /* Check for conversion errors */
    if (endptr == value || *endptr != '\0') {
        return default_value;
    }
    
    /* Check for overflow */
    if (result < 0 || result > INT_MAX) {
        return default_value;
    }
    
    return (int)result;
}

int buckets_s3_list_objects_v1(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (req->bucket[0] == '\0') {
        return buckets_s3_xml_error(res, "InvalidBucketName",
                                     "Bucket name is required",
                                     "/");
    }
    
    /* Parse query parameters */
    const char *prefix = get_query_param(req, "prefix");
    const char *marker = get_query_param(req, "marker");
    (void)get_query_param(req, "delimiter"); /* TODO: implement delimiter support */
    int max_keys = get_query_param_int(req, "max-keys", 1000);
    
    if (max_keys <= 0) {
        max_keys = 1000;
    }
    if (max_keys > 1000) {
        max_keys = 1000; /* S3 limit */
    }
    
    /* Check if bucket exists */
    char bucket_path[2048];
    snprintf(bucket_path, sizeof(bucket_path), "/tmp/buckets-data/%s", req->bucket);
    
    DIR *dir = opendir(bucket_path);
    if (!dir) {
        return buckets_s3_xml_error(res, "NoSuchBucket",
                                     "The specified bucket does not exist",
                                     req->bucket);
    }
    
    /* Build XML response */
    char xml_body[65536]; /* Large buffer for many objects */
    int offset = snprintf(xml_body, sizeof(xml_body),
                          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
                          "  <Name>%s</Name>\n",
                          req->bucket);
    
    if (prefix) {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <Prefix>%s</Prefix>\n", prefix);
    } else {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <Prefix></Prefix>\n");
    }
    
    if (marker) {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <Marker>%s</Marker>\n", marker);
    } else {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <Marker></Marker>\n");
    }
    
    offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                       "  <MaxKeys>%d</MaxKeys>\n", max_keys);
    
    /* Collect and filter objects */
    struct dirent *entry;
    int count = 0;
    bool is_truncated = false;
    char next_marker[1024] = {0};
    bool past_marker = (marker == NULL || marker[0] == '\0');
    
    while ((entry = readdir(dir)) != NULL && count < max_keys) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Apply marker filter (skip until we pass the marker) */
        if (!past_marker) {
            if (strcmp(entry->d_name, marker) <= 0) {
                continue;
            }
            past_marker = true;
        }
        
        /* Apply prefix filter */
        if (prefix && strncmp(entry->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        
        /* Get object stats */
        char object_path[3072];
        snprintf(object_path, sizeof(object_path), "%s/%s", bucket_path, entry->d_name);
        
        struct stat st;
        if (stat(object_path, &st) != 0) {
            continue;
        }
        
        /* Skip directories (in case there are any) */
        if (S_ISDIR(st.st_mode)) {
            continue;
        }
        
        /* Add object to XML */
        char timestamp[64];
        struct tm *tm_info = gmtime(&st.st_mtime);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
        
        /* Calculate ETag (MD5 of file content) - for now, use simplified version */
        char etag[33] = {0};
        snprintf(etag, sizeof(etag), "\"%016lx\"", (unsigned long)st.st_mtime);
        
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <Contents>\n"
                           "    <Key>%s</Key>\n"
                           "    <LastModified>%s</LastModified>\n"
                           "    <ETag>%s</ETag>\n"
                           "    <Size>%ld</Size>\n"
                           "    <StorageClass>STANDARD</StorageClass>\n"
                           "  </Contents>\n",
                           entry->d_name, timestamp, etag, (long)st.st_size);
        
        count++;
        strncpy(next_marker, entry->d_name, sizeof(next_marker) - 1);
        
        if (offset >= (int)sizeof(xml_body) - 1000) {
            buckets_warn("LIST objects: XML buffer nearly full");
            is_truncated = true;
            break;
        }
    }
    
    /* Check if there are more results */
    if (!is_truncated && entry != NULL) {
        is_truncated = true;
    }
    
    closedir(dir);
    
    /* Add truncation info */
    offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                       "  <IsTruncated>%s</IsTruncated>\n",
                       is_truncated ? "true" : "false");
    
    if (is_truncated && next_marker[0] != '\0') {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <NextMarker>%s</NextMarker>\n", next_marker);
    }
    
    /* Close XML */
    offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                       "</ListBucketResult>\n");
    
    buckets_debug("LIST objects v1: %s (count=%d, truncated=%s)",
                  req->bucket, count, is_truncated ? "true" : "false");
    
    res->status_code = 200;
    res->body = buckets_strdup(xml_body);
    res->body_len = strlen(xml_body);
    
    return BUCKETS_OK;
}

int buckets_s3_list_objects_v2(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (req->bucket[0] == '\0') {
        return buckets_s3_xml_error(res, "InvalidBucketName",
                                     "Bucket name is required",
                                     "/");
    }
    
    /* Parse query parameters */
    const char *prefix = get_query_param(req, "prefix");
    const char *continuation_token = get_query_param(req, "continuation-token");
    const char *start_after = get_query_param(req, "start-after");
    (void)get_query_param(req, "delimiter"); /* TODO: implement delimiter support */
    int max_keys = get_query_param_int(req, "max-keys", 1000);
    
    if (max_keys <= 0) {
        max_keys = 1000;
    }
    if (max_keys > 1000) {
        max_keys = 1000;
    }
    
    /* continuation-token takes precedence over start-after */
    const char *marker = continuation_token ? continuation_token : start_after;
    
    /* Check if bucket exists */
    char bucket_path[2048];
    snprintf(bucket_path, sizeof(bucket_path), "/tmp/buckets-data/%s", req->bucket);
    
    DIR *dir = opendir(bucket_path);
    if (!dir) {
        return buckets_s3_xml_error(res, "NoSuchBucket",
                                     "The specified bucket does not exist",
                                     req->bucket);
    }
    
    /* Build XML response (v2 format) */
    char xml_body[65536];
    int offset = snprintf(xml_body, sizeof(xml_body),
                          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
                          "  <Name>%s</Name>\n",
                          req->bucket);
    
    if (prefix) {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <Prefix>%s</Prefix>\n", prefix);
    } else {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <Prefix></Prefix>\n");
    }
    
    offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                       "  <KeyCount>0</KeyCount>\n" /* Will update later */
                       "  <MaxKeys>%d</MaxKeys>\n", max_keys);
    
    if (continuation_token) {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <ContinuationToken>%s</ContinuationToken>\n",
                           continuation_token);
    }
    
    if (start_after) {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <StartAfter>%s</StartAfter>\n", start_after);
    }
    
    /* Collect and filter objects */
    struct dirent *entry;
    int count = 0;
    bool is_truncated = false;
    char next_token[1024] = {0};
    bool past_marker = (marker == NULL || marker[0] == '\0');
    
    /* Save position to update KeyCount later */
    int key_count_pos = offset;
    
    while ((entry = readdir(dir)) != NULL && count < max_keys) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Apply marker filter */
        if (!past_marker) {
            if (strcmp(entry->d_name, marker) <= 0) {
                continue;
            }
            past_marker = true;
        }
        
        /* Apply prefix filter */
        if (prefix && strncmp(entry->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        
        /* Get object stats */
        char object_path[3072];
        snprintf(object_path, sizeof(object_path), "%s/%s", bucket_path, entry->d_name);
        
        struct stat st;
        if (stat(object_path, &st) != 0) {
            continue;
        }
        
        if (S_ISDIR(st.st_mode)) {
            continue;
        }
        
        /* Add object to XML */
        char timestamp[64];
        struct tm *tm_info = gmtime(&st.st_mtime);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
        
        char etag[33] = {0};
        snprintf(etag, sizeof(etag), "\"%016lx\"", (unsigned long)st.st_mtime);
        
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <Contents>\n"
                           "    <Key>%s</Key>\n"
                           "    <LastModified>%s</LastModified>\n"
                           "    <ETag>%s</ETag>\n"
                           "    <Size>%ld</Size>\n"
                           "    <StorageClass>STANDARD</StorageClass>\n"
                           "  </Contents>\n",
                           entry->d_name, timestamp, etag, (long)st.st_size);
        
        count++;
        strncpy(next_token, entry->d_name, sizeof(next_token) - 1);
        
        if (offset >= (int)sizeof(xml_body) - 1000) {
            is_truncated = true;
            break;
        }
    }
    
    if (!is_truncated && entry != NULL) {
        is_truncated = true;
    }
    
    closedir(dir);
    
    /* Add truncation info */
    offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                       "  <IsTruncated>%s</IsTruncated>\n",
                       is_truncated ? "true" : "false");
    
    if (is_truncated && next_token[0] != '\0') {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <NextContinuationToken>%s</NextContinuationToken>\n",
                           next_token);
    }
    
    /* Close XML */
    offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                       "</ListBucketResult>\n");
    
    /* Update KeyCount in the XML (a bit hacky but works) */
    char temp[65536];
    memcpy(temp, xml_body, key_count_pos);
    int updated_offset = snprintf(temp + key_count_pos, sizeof(temp) - key_count_pos,
                                   "  <KeyCount>%d</KeyCount>\n", count);
    /* Skip the old KeyCount line and copy the rest */
    const char *rest = strstr(xml_body + key_count_pos, "<MaxKeys>");
    if (rest) {
        strcpy(temp + key_count_pos + updated_offset, rest);
    }
    memcpy(xml_body, temp, sizeof(xml_body));
    
    buckets_debug("LIST objects v2: %s (count=%d, truncated=%s)",
                  req->bucket, count, is_truncated ? "true" : "false");
    
    res->status_code = 200;
    res->body = buckets_strdup(xml_body);
    res->body_len = strlen(xml_body);
    
    return BUCKETS_OK;
}
