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
