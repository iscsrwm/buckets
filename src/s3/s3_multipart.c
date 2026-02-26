/**
 * S3 Multipart Upload Operations
 * 
 * Implements S3-compatible multipart upload functionality for large objects.
 * 
 * Storage structure:
 * /tmp/buckets-data/{bucket}/.multipart/{uploadId}/
 *   ├── metadata.json   - Upload metadata (bucket, key, initiated time)
 *   └── parts/          - Part files
 *       ├── part.1
 *       ├── part.2
 *       └── ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>
#include <uuid/uuid.h>

#include "buckets.h"
#include "buckets_s3.h"

#include "../../third_party/cJSON/cJSON.h"

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/**
 * Get multipart upload directory path
 */
static void get_multipart_dir(const char *bucket, const char *upload_id, char *path, size_t path_size)
{
    snprintf(path, path_size, "/tmp/buckets-data/%s/.multipart/%s", bucket, upload_id);
}

/**
 * Get part file path
 */
static void get_part_path(const char *bucket, const char *upload_id, int part_number, 
                          char *path, size_t path_size)
{
    snprintf(path, path_size, "/tmp/buckets-data/%s/.multipart/%s/parts/part.%d",
             bucket, upload_id, part_number);
}

/**
 * Get metadata file path
 */
static void get_metadata_path(const char *bucket, const char *upload_id, 
                             char *path, size_t path_size)
{
    snprintf(path, path_size, "/tmp/buckets-data/%s/.multipart/%s/metadata.json",
             bucket, upload_id);
}

/**
 * Generate upload ID (UUID)
 */
static void generate_upload_id(char *upload_id, size_t size)
{
    uuid_t uuid;
    uuid_generate(uuid);
    
    /* Convert to string without dashes for cleaner IDs */
    char uuid_str[37];
    uuid_unparse_lower(uuid, uuid_str);
    
    /* Remove dashes */
    int j = 0;
    for (int i = 0; uuid_str[i] && j < (int)size - 1; i++) {
        if (uuid_str[i] != '-') {
            upload_id[j++] = uuid_str[i];
        }
    }
    upload_id[j] = '\0';
}

/**
 * Create multipart upload directory structure
 */
static int create_upload_dirs(const char *bucket, const char *upload_id)
{
    char path[2048];
    
    /* Create .multipart directory */
    snprintf(path, sizeof(path), "/tmp/buckets-data/%s/.multipart", bucket);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        buckets_error("Failed to create .multipart directory: %s", path);
        return BUCKETS_ERR_IO;
    }
    
    /* Create upload ID directory */
    get_multipart_dir(bucket, upload_id, path, sizeof(path));
    if (mkdir(path, 0755) != 0) {
        buckets_error("Failed to create upload directory: %s", path);
        return BUCKETS_ERR_IO;
    }
    
    /* Create parts subdirectory */
    snprintf(path, sizeof(path), "/tmp/buckets-data/%s/.multipart/%s/parts",
             bucket, upload_id);
    if (mkdir(path, 0755) != 0) {
        buckets_error("Failed to create parts directory: %s", path);
        return BUCKETS_ERR_IO;
    }
    
    return BUCKETS_OK;
}

/**
 * Save upload metadata to JSON file
 */
static int save_upload_metadata(const char *bucket, const char *key, const char *upload_id)
{
    char path[2048];
    get_metadata_path(bucket, upload_id, path, sizeof(path));
    
    /* Create JSON metadata */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return BUCKETS_ERR_NOMEM;
    }
    
    cJSON_AddStringToObject(root, "bucket", bucket);
    cJSON_AddStringToObject(root, "key", key);
    cJSON_AddStringToObject(root, "uploadId", upload_id);
    cJSON_AddNumberToObject(root, "initiated", (double)time(NULL));
    
    /* Write to file */
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        return BUCKETS_ERR_NOMEM;
    }
    
    FILE *fp = fopen(path, "w");
    if (!fp) {
        buckets_free(json_str);
        buckets_error("Failed to create metadata file: %s", path);
        return BUCKETS_ERR_IO;
    }
    
    fputs(json_str, fp);
    fclose(fp);
    buckets_free(json_str);
    
    return BUCKETS_OK;
}

/**
 * Load upload metadata from JSON file
 */
static cJSON* load_upload_metadata(const char *bucket, const char *upload_id)
{
    char path[2048];
    get_metadata_path(bucket, upload_id, path, sizeof(path));
    
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }
    
    /* Read file */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char *json_str = buckets_malloc(fsize + 1);
    if (!json_str) {
        fclose(fp);
        return NULL;
    }
    
    size_t bytes_read = fread(json_str, 1, fsize, fp);
    json_str[bytes_read] = '\0';
    fclose(fp);
    
    cJSON *root = cJSON_Parse(json_str);
    buckets_free(json_str);
    
    return root;
}

/**
 * Recursively remove directory
 */
static void remove_directory(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char full_path[3072];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        
        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                remove_directory(full_path);
            } else {
                unlink(full_path);
            }
        }
    }
    closedir(dir);
    rmdir(path);
}

/* ===================================================================
 * Multipart Upload Operations
 * ===================================================================*/

int buckets_s3_initiate_multipart_upload(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (req->bucket[0] == '\0' || req->key[0] == '\0') {
        return buckets_s3_xml_error(res, "InvalidRequest",
                                     "Bucket and key are required",
                                     "/");
    }
    
    /* Validate bucket exists */
    char bucket_path[2048];
    snprintf(bucket_path, sizeof(bucket_path), "/tmp/buckets-data/%s", req->bucket);
    
    struct stat st;
    if (stat(bucket_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return buckets_s3_xml_error(res, "NoSuchBucket",
                                     "The specified bucket does not exist",
                                     req->bucket);
    }
    
    /* Generate upload ID */
    char upload_id[64];
    generate_upload_id(upload_id, sizeof(upload_id));
    
    buckets_debug("Initiating multipart upload: bucket=%s, key=%s, uploadId=%s",
                  req->bucket, req->key, upload_id);
    
    /* Create upload directory structure */
    int ret = create_upload_dirs(req->bucket, upload_id);
    if (ret != BUCKETS_OK) {
        return buckets_s3_xml_error(res, "InternalError",
                                     "Failed to create upload directory",
                                     req->key);
    }
    
    /* Save metadata */
    ret = save_upload_metadata(req->bucket, req->key, upload_id);
    if (ret != BUCKETS_OK) {
        /* Clean up on failure */
        char upload_dir[2048];
        get_multipart_dir(req->bucket, upload_id, upload_dir, sizeof(upload_dir));
        remove_directory(upload_dir);
        
        return buckets_s3_xml_error(res, "InternalError",
                                     "Failed to save upload metadata",
                                     req->key);
    }
    
    /* Generate XML response */
    char xml_body[4096];
    snprintf(xml_body, sizeof(xml_body),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<InitiateMultipartUploadResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
             "  <Bucket>%s</Bucket>\n"
             "  <Key>%s</Key>\n"
             "  <UploadId>%s</UploadId>\n"
             "</InitiateMultipartUploadResult>\n",
             req->bucket, req->key, upload_id);
    
    res->status_code = 200;
    res->body = buckets_strdup(xml_body);
    res->body_len = strlen(xml_body);
    
    buckets_info("Multipart upload initiated: %s/%s (uploadId=%s)",
                 req->bucket, req->key, upload_id);
    
    return BUCKETS_OK;
}

int buckets_s3_upload_part(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Get uploadId and partNumber from query parameters */
    const char *upload_id = NULL;
    const char *part_number_str = NULL;
    
    for (int i = 0; i < req->query_count; i++) {
        if (strcmp(req->query_params_keys[i], "uploadId") == 0) {
            upload_id = req->query_params_values[i];
        } else if (strcmp(req->query_params_keys[i], "partNumber") == 0) {
            part_number_str = req->query_params_values[i];
        }
    }
    
    if (!upload_id || !part_number_str) {
        return buckets_s3_xml_error(res, "InvalidRequest",
                                     "uploadId and partNumber are required",
                                     req->key);
    }
    
    int part_number = atoi(part_number_str);
    if (part_number < 1 || part_number > 10000) {
        return buckets_s3_xml_error(res, "InvalidPartNumber",
                                     "Part number must be between 1 and 10000",
                                     req->key);
    }
    
    /* Verify upload exists */
    cJSON *metadata = load_upload_metadata(req->bucket, upload_id);
    if (!metadata) {
        return buckets_s3_xml_error(res, "NoSuchUpload",
                                     "The specified upload does not exist",
                                     upload_id);
    }
    
    /* Verify bucket and key match */
    const cJSON *meta_bucket = cJSON_GetObjectItem(metadata, "bucket");
    const cJSON *meta_key = cJSON_GetObjectItem(metadata, "key");
    
    if (!meta_bucket || !meta_key ||
        strcmp(meta_bucket->valuestring, req->bucket) != 0 ||
        strcmp(meta_key->valuestring, req->key) != 0) {
        cJSON_Delete(metadata);
        return buckets_s3_xml_error(res, "NoSuchUpload",
                                     "Upload ID does not match bucket/key",
                                     upload_id);
    }
    cJSON_Delete(metadata);
    
    buckets_debug("Uploading part: bucket=%s, key=%s, uploadId=%s, partNumber=%d, size=%zu",
                  req->bucket, req->key, upload_id, part_number, req->body_len);
    
    /* Write part to file */
    char part_path[2048];
    get_part_path(req->bucket, upload_id, part_number, part_path, sizeof(part_path));
    
    FILE *fp = fopen(part_path, "wb");
    if (!fp) {
        buckets_error("Failed to create part file: %s", part_path);
        return buckets_s3_xml_error(res, "InternalError",
                                     "Failed to write part",
                                     req->key);
    }
    
    if (req->body_len > 0) {
        size_t written = fwrite(req->body, 1, req->body_len, fp);
        if (written != req->body_len) {
            fclose(fp);
            unlink(part_path);
            buckets_error("Failed to write part data: %s", part_path);
            return buckets_s3_xml_error(res, "InternalError",
                                         "Failed to write part data",
                                         req->key);
        }
    }
    fclose(fp);
    
    /* Calculate ETag for the part */
    char etag[33];  /* MD5 is 32 hex chars */
    buckets_s3_calculate_etag(req->body, req->body_len, etag);
    
    buckets_info("Part uploaded: %s/%s part %d (etag=%s, size=%zu)",
                 req->bucket, req->key, part_number, etag, req->body_len);
    
    /* Set ETag header in response (with quotes) */
    snprintf(res->etag, sizeof(res->etag), "\"%s\"", etag);
    res->status_code = 200;
    
    return BUCKETS_OK;
}

int buckets_s3_complete_multipart_upload(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    /* TODO: Implement in next session */
    (void)req;
    return buckets_s3_xml_error(res, "NotImplemented",
                                 "CompleteMultipartUpload not yet implemented",
                                 "/");
}

int buckets_s3_abort_multipart_upload(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    /* TODO: Implement in next session */
    (void)req;
    return buckets_s3_xml_error(res, "NotImplemented",
                                 "AbortMultipartUpload not yet implemented",
                                 "/");
}

int buckets_s3_list_parts(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    /* TODO: Implement in next session */
    (void)req;
    return buckets_s3_xml_error(res, "NotImplemented",
                                 "ListParts not yet implemented",
                                 "/");
}
