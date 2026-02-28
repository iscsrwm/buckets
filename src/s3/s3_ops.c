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
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <openssl/evp.h>

#include "buckets.h"
#include "buckets_s3.h"
#include "buckets_storage.h"
#include "buckets_cluster.h"
#include "buckets_registry.h"

#define MD5_DIGEST_LENGTH 16

/* ===================================================================
 * Utility Functions
 * ===================================================================*/

/**
 * Get bucket path (try multi-disk first, fallback to single-disk)
 * Returns 0 if found, -1 if not found
 */
static int get_bucket_path(const char *bucket, char *path, size_t path_len)
{
    extern int buckets_get_data_dir(char *data_dir, size_t size);
    char data_dir[512];
    if (buckets_get_data_dir(data_dir, sizeof(data_dir)) != 0) {
        snprintf(data_dir, sizeof(data_dir), "/tmp/buckets-data");
    }
    
    /* Try multi-disk path first */
    snprintf(path, path_len, "%s/disk1/%s", data_dir, bucket);
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0;
    }
    
    /* Try single-disk path */
    snprintf(path, path_len, "%s/%s", data_dir, bucket);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0;
    }
    
    return -1;  /* Not found */
}

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
    
    /* Use distributed storage layer */
    const char *content_type = req->content_type[0] != '\0' ? req->content_type : NULL;
    const void *data = req->body ? req->body : "";
    size_t size = req->body_len;
    
    buckets_info("S3 PUT: body_len=%zu, body_ptr=%p", size, (void*)data);
    if (size >= 4) {
        const u8 *bytes = (const u8 *)data;
        buckets_info("S3 PUT: first 4 bytes: %02x %02x %02x %02x", 
                    bytes[0], bytes[1], bytes[2], bytes[3]);
    }
    if (size > 131072) {
        const u8 *bytes = (const u8 *)data;
        buckets_info("S3 PUT: bytes at 131072: %02x %02x %02x %02x",
                    bytes[131072], bytes[131073], bytes[131074], bytes[131075]);
    }
    
    int ret = buckets_put_object(req->bucket, req->key, data, size, content_type);
    if (ret != 0) {
        buckets_error("Failed to write object to distributed storage: %s/%s", 
                     req->bucket, req->key);
        buckets_s3_xml_error(res, "InternalError",
                            "Failed to write object to storage",
                            req->key);
        return BUCKETS_OK;
    }
    
    /* Calculate ETag */
    if (req->body && req->body_len > 0) {
        buckets_s3_calculate_etag(req->body, req->body_len, res->etag);
    } else {
        /* Empty object */
        buckets_s3_calculate_etag("", 0, res->etag);
    }
    
    /* Generate success response */
    buckets_s3_xml_success(res, "PutObjectResult");
    
    buckets_info("PUT object: %s/%s (%zu bytes, ETag: %s) - written to distributed storage",
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
    
    /* Use distributed storage layer */
    void *object_data = NULL;
    size_t object_size = 0;
    
    int ret = buckets_get_object(req->bucket, req->key, &object_data, &object_size);
    if (ret != 0) {
        /* Object not found or error */
        buckets_s3_xml_error(res, "NoSuchKey",
                            "The specified key does not exist",
                            req->key);
        return BUCKETS_OK;
    }
    
    /* Calculate ETag */
    buckets_s3_calculate_etag(object_data, object_size, res->etag);
    
    /* Set response */
    res->status_code = 200;
    res->body = object_data;  /* Caller owns this memory */
    res->body_len = object_size;
    res->content_length = object_size;
    
    /* Set content type (default to application/octet-stream) */
    if (req->content_type[0] != '\0') {
        strncpy(res->content_type, req->content_type, sizeof(res->content_type) - 1);
        res->content_type[sizeof(res->content_type) - 1] = '\0';
    } else {
        strcpy(res->content_type, "application/octet-stream");
    }
    
    /* Format last modified time */
    buckets_s3_format_timestamp(time(NULL), res->last_modified);
    
    buckets_info("GET object: %s/%s (%zu bytes, ETag: %s) - read from distributed storage",
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
    
    /* Use distributed delete (handles multi-disk and RPC deletion) */
    extern int buckets_distributed_delete_object(const char *bucket, const char *object);
    int ret = buckets_distributed_delete_object(req->bucket, req->key);
    
    if (ret != 0) {
        /* Object might not exist - S3 DELETE is idempotent, so we still return 204 */
        buckets_debug("DELETE object (distributed): %s/%s - not found or error", 
                      req->bucket, req->key);
    } else {
        buckets_info("DELETE object (distributed): %s/%s - deleted successfully", 
                     req->bucket, req->key);
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
    
    /* Get data directory */
    extern int buckets_get_data_dir(char *data_dir, size_t size);
    char data_dir[512];
    if (buckets_get_data_dir(data_dir, sizeof(data_dir)) != 0) {
        snprintf(data_dir, sizeof(data_dir), "/tmp/buckets-data");
    }
    
    /* Create bucket directory on all disks (try disk1 first, then fallback to data_dir root) */
    char bucket_path[2048];
    snprintf(bucket_path, sizeof(bucket_path), "%s/disk1/%s", data_dir, req->bucket);
    
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
    } else {
        /* Try single-disk mode path */
        snprintf(bucket_path, sizeof(bucket_path), "%s/%s", data_dir, req->bucket);
        if (stat(bucket_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Bucket exists in single-disk mode */
            return buckets_s3_xml_error(res, "BucketAlreadyOwnedByYou",
                                         "Your previous request to create the named bucket succeeded and you already own it",
                                         req->bucket);
        }
    }
    
    /* Create bucket directory (try multi-disk first) */
    snprintf(bucket_path, sizeof(bucket_path), "%s/disk1/%s", data_dir, req->bucket);
    if (mkdir(bucket_path, 0755) != 0 && errno != EEXIST) {
        /* Multi-disk failed, try single-disk mode */
        snprintf(bucket_path, sizeof(bucket_path), "%s/%s", data_dir, req->bucket);
        if (mkdir(bucket_path, 0755) != 0 && errno != EEXIST) {
            buckets_error("Failed to create bucket directory: %s", bucket_path);
            return buckets_s3_xml_error(res, "InternalError",
                                         "Failed to create bucket",
                                         req->bucket);
        }
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
    if (get_bucket_path(req->bucket, bucket_path, sizeof(bucket_path)) != 0) {
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
    if (get_bucket_path(req->bucket, bucket_path, sizeof(bucket_path)) != 0) {
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
    
    /* Get data directory */
    extern int buckets_get_data_dir(char *data_dir, size_t size);
    char data_dir[512];
    if (buckets_get_data_dir(data_dir, sizeof(data_dir)) != 0) {
        snprintf(data_dir, sizeof(data_dir), "/tmp/buckets-data");
    }
    
    /* Try multi-disk mode first (disk1 subdirectory) */
    char buckets_dir[1024];
    snprintf(buckets_dir, sizeof(buckets_dir), "%s/disk1", data_dir);
    DIR *dir = opendir(buckets_dir);
    if (!dir) {
        /* Try single-disk mode */
        snprintf(buckets_dir, sizeof(buckets_dir), "%s", data_dir);
        dir = opendir(buckets_dir);
    }
    
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
        snprintf(bucket_path, sizeof(bucket_path), "%s/%s", buckets_dir, entry->d_name);
        
        struct stat st;
        if (stat(bucket_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        
        /* Skip hidden directories and special dirs */
        if (entry->d_name[0] == '.') {
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
 * Object entry for sorting
 */
typedef struct {
    char name[1024];
    struct stat st;
} object_entry_t;

/**
 * Comparison function for qsort (lexicographic order)
 */
static int compare_objects(const void *a, const void *b)
{
    const object_entry_t *obj_a = (const object_entry_t *)a;
    const object_entry_t *obj_b = (const object_entry_t *)b;
    return strcmp(obj_a->name, obj_b->name);
}

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
 * Collect and sort objects from registry
 * Returns number of objects collected, or -1 on error
 * 
 * This is the primary method for listing objects - uses the location registry
 * to find all objects in a bucket, which works with hash-based storage paths.
 */
static int collect_objects_from_registry(const char *bucket, 
                                          const char *prefix,
                                          object_entry_t **objects,
                                          int *capacity)
{
    buckets_object_location_t **locations = NULL;
    size_t location_count = 0;
    
    /* Query registry for all objects in this bucket */
    int ret = buckets_registry_list(bucket, prefix, 0, &locations, &location_count);
    if (ret != 0 || locations == NULL) {
        /* Registry query failed - bucket may not exist or no objects */
        if (location_count == 0) {
            /* Empty bucket - return empty list, not error */
            int cap = 1;
            object_entry_t *objs = buckets_calloc(cap, sizeof(object_entry_t));
            if (!objs) return -1;
            *objects = objs;
            *capacity = cap;
            return 0;
        }
        return -1;
    }
    
    /* Convert registry locations to object entries */
    int cap = (int)location_count > 0 ? (int)location_count : 1;
    object_entry_t *objs = buckets_calloc(cap, sizeof(object_entry_t));
    if (!objs) {
        /* Free locations */
        for (size_t i = 0; i < location_count; i++) {
            buckets_registry_location_free(locations[i]);
        }
        buckets_free(locations);
        return -1;
    }
    
    int count = 0;
    for (size_t i = 0; i < location_count; i++) {
        if (!locations[i] || !locations[i]->object) {
            buckets_registry_location_free(locations[i]);
            continue;
        }
        
        /* Skip duplicates (keep only "latest" version) */
        if (locations[i]->version_id && 
            strcmp(locations[i]->version_id, "latest") != 0) {
            buckets_registry_location_free(locations[i]);
            continue;
        }
        
        /* Copy object info */
        strncpy(objs[count].name, locations[i]->object, sizeof(objs[count].name) - 1);
        objs[count].name[sizeof(objs[count].name) - 1] = '\0';
        
        /* Create synthetic stat from registry info */
        memset(&objs[count].st, 0, sizeof(struct stat));
        objs[count].st.st_size = (off_t)locations[i]->size;
        objs[count].st.st_mtime = locations[i]->mod_time;
        objs[count].st.st_mode = S_IFREG | 0644;
        
        count++;
        buckets_registry_location_free(locations[i]);
    }
    buckets_free(locations);
    
    /* Sort lexicographically */
    if (count > 0) {
        qsort(objs, count, sizeof(object_entry_t), compare_objects);
    }
    
    *objects = objs;
    *capacity = cap;
    
    buckets_debug("collect_objects_from_registry: bucket=%s, prefix=%s, found=%d",
                  bucket, prefix ? prefix : "(none)", count);
    
    return count;
}

/**
 * Collect and sort objects from directory (fallback for non-registry buckets)
 * Returns number of objects collected, or -1 on error
 */
static int collect_sorted_objects(const char *bucket_path, 
                                   const char *prefix,
                                   object_entry_t **objects,
                                   int *capacity)
{
    DIR *dir = opendir(bucket_path);
    if (!dir) {
        return -1;
    }
    
    int count = 0;
    int cap = 100; /* Initial capacity */
    object_entry_t *objs = buckets_calloc(cap, sizeof(object_entry_t));
    if (!objs) {
        closedir(dir);
        return -1;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        /* Apply prefix filter */
        if (prefix && strncmp(entry->d_name, prefix, strlen(prefix)) != 0) {
            continue;
        }
        
        /* Get object stats */
        char object_path[3072];
        snprintf(object_path, sizeof(object_path), "%s/%s", bucket_path, entry->d_name);
        
        struct stat st;
        if (stat(object_path, &st) != 0 || S_ISDIR(st.st_mode)) {
            continue;
        }
        
        /* Expand array if needed */
        if (count >= cap) {
            cap *= 2;
            object_entry_t *new_objs = buckets_realloc(objs, cap * sizeof(object_entry_t));
            if (!new_objs) {
                buckets_free(objs);
                closedir(dir);
                return -1;
            }
            objs = new_objs;
        }
        
        /* Add object */
        strncpy(objs[count].name, entry->d_name, sizeof(objs[count].name) - 1);
        objs[count].name[sizeof(objs[count].name) - 1] = '\0';
        objs[count].st = st;
        count++;
    }
    closedir(dir);
    
    /* Sort lexicographically */
    if (count > 0) {
        qsort(objs, count, sizeof(object_entry_t), compare_objects);
    }
    
    *objects = objs;
    *capacity = cap;
    return count;
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
    
    /* Collect objects from registry (primary) or filesystem (fallback) */
    object_entry_t *objects = NULL;
    int capacity = 0;
    int total_objects = collect_objects_from_registry(req->bucket, prefix, &objects, &capacity);
    
    if (total_objects < 0) {
        /* Registry failed - try filesystem fallback */
        char bucket_path[2048];
        snprintf(bucket_path, sizeof(bucket_path), "/tmp/buckets-data/%s", req->bucket);
        total_objects = collect_sorted_objects(bucket_path, prefix, &objects, &capacity);
        
        if (total_objects < 0) {
            return buckets_s3_xml_error(res, "NoSuchBucket",
                                         "The specified bucket does not exist",
                                         req->bucket);
        }
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
    
    /* Iterate through sorted objects, applying marker filter and max-keys limit */
    int count = 0;
    bool is_truncated = false;
    char next_marker[1024] = {0};
    
    for (int i = 0; i < total_objects && count < max_keys; i++) {
        /* Apply marker filter (skip until we pass the marker) */
        if (marker && strcmp(objects[i].name, marker) <= 0) {
            continue;
        }
        
        /* Format timestamp */
        char timestamp[64];
        struct tm *tm_info = gmtime(&objects[i].st.st_mtime);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
        
        /* Generate ETag from size + mtime (registry doesn't store actual ETag) */
        char etag[64];
        snprintf(etag, sizeof(etag), "\"%016lx%08lx\"", 
                 (unsigned long)objects[i].st.st_size,
                 (unsigned long)objects[i].st.st_mtime);
        
        /* Add object to XML */
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <Contents>\n"
                           "    <Key>%s</Key>\n"
                           "    <LastModified>%s</LastModified>\n"
                           "    <ETag>%s</ETag>\n"
                           "    <Size>%ld</Size>\n"
                           "    <StorageClass>STANDARD</StorageClass>\n"
                           "  </Contents>\n",
                           objects[i].name, timestamp, etag, (long)objects[i].st.st_size);
        
        count++;
        strncpy(next_marker, objects[i].name, sizeof(next_marker) - 1);
        
        if (offset >= (int)sizeof(xml_body) - 1000) {
            buckets_warn("LIST objects: XML buffer nearly full");
            is_truncated = true;
            break;
        }
    }
    
    /* Check if there are more results beyond what we returned */
    if (!is_truncated) {
        /* Find the index of the last returned object */
        int last_idx = -1;
        for (int i = 0; i < total_objects; i++) {
            if (marker && strcmp(objects[i].name, marker) <= 0) {
                continue;
            }
            last_idx++;
            if (last_idx >= count - 1) {
                /* Check if there's another object after this one */
                if (i + 1 < total_objects) {
                    is_truncated = true;
                }
                break;
            }
        }
    }
    
    buckets_free(objects);
    
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
    
    /* Collect objects from registry (primary) or filesystem (fallback) */
    object_entry_t *objects = NULL;
    int capacity = 0;
    int total_objects = collect_objects_from_registry(req->bucket, prefix, &objects, &capacity);
    
    if (total_objects < 0) {
        /* Registry failed - try filesystem fallback */
        char bucket_path[2048];
        snprintf(bucket_path, sizeof(bucket_path), "/tmp/buckets-data/%s", req->bucket);
        total_objects = collect_sorted_objects(bucket_path, prefix, &objects, &capacity);
        
        if (total_objects < 0) {
            return buckets_s3_xml_error(res, "NoSuchBucket",
                                         "The specified bucket does not exist",
                                         req->bucket);
        }
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
    
    /* Iterate through sorted objects, applying marker filter and max-keys limit */
    int count = 0;
    bool is_truncated = false;
    char next_token[1024] = {0};
    
    /* First pass to count objects for KeyCount */
    for (int i = 0; i < total_objects && count < max_keys; i++) {
        /* Apply marker filter (skip until we pass the marker) */
        if (marker && strcmp(objects[i].name, marker) <= 0) {
            continue;
        }
        count++;
    }
    
    /* Now output KeyCount and MaxKeys */
    offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                       "  <KeyCount>%d</KeyCount>\n"
                       "  <MaxKeys>%d</MaxKeys>\n", count, max_keys);
    
    if (continuation_token) {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <ContinuationToken>%s</ContinuationToken>\n",
                           continuation_token);
    }
    
    if (start_after) {
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <StartAfter>%s</StartAfter>\n", start_after);
    }
    
    /* Second pass to generate XML for objects */
    count = 0; /* Reset count */
    for (int i = 0; i < total_objects && count < max_keys; i++) {
        /* Apply marker filter */
        if (marker && strcmp(objects[i].name, marker) <= 0) {
            continue;
        }
        
        /* Format timestamp */
        char timestamp[64];
        struct tm *tm_info = gmtime(&objects[i].st.st_mtime);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
        
        /* Generate ETag from size + mtime (registry doesn't store actual ETag) */
        char etag[64];
        snprintf(etag, sizeof(etag), "\"%016lx%08lx\"", 
                 (unsigned long)objects[i].st.st_size,
                 (unsigned long)objects[i].st.st_mtime);
        
        /* Add object to XML */
        offset += snprintf(xml_body + offset, sizeof(xml_body) - offset,
                           "  <Contents>\n"
                           "    <Key>%s</Key>\n"
                           "    <LastModified>%s</LastModified>\n"
                           "    <ETag>%s</ETag>\n"
                           "    <Size>%ld</Size>\n"
                           "    <StorageClass>STANDARD</StorageClass>\n"
                           "  </Contents>\n",
                           objects[i].name, timestamp, etag, (long)objects[i].st.st_size);
        
        count++;
        strncpy(next_token, objects[i].name, sizeof(next_token) - 1);
        
        if (offset >= (int)sizeof(xml_body) - 1000) {
            buckets_warn("LIST objects: XML buffer nearly full");
            is_truncated = true;
            break;
        }
    }
    
    /* Check if there are more results beyond what we returned */
    if (!is_truncated) {
        /* Find the index of the last returned object */
        int last_idx = -1;
        for (int i = 0; i < total_objects; i++) {
            if (marker && strcmp(objects[i].name, marker) <= 0) {
                continue;
            }
            last_idx++;
            if (last_idx >= count - 1) {
                /* Check if there's another object after this one */
                if (i + 1 < total_objects) {
                    is_truncated = true;
                }
                break;
            }
        }
    }
    
    buckets_free(objects);
    
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
    
    buckets_debug("LIST objects v2: %s (count=%d, truncated=%s)",
                  req->bucket, count, is_truncated ? "true" : "false");
    
    res->status_code = 200;
    res->body = buckets_strdup(xml_body);
    res->body_len = strlen(xml_body);
    
    return BUCKETS_OK;
}
