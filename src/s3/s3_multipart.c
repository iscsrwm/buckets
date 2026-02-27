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
#include "buckets_storage.h"

#include "../../third_party/cJSON/cJSON.h"

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/**
 * Get multipart upload directory path
 */
static void get_multipart_dir(const char *bucket, const char *upload_id, char *path, size_t path_size)
{
    /* Get data directory from storage config */
    extern int buckets_get_data_dir(char *data_dir, size_t size);
    char data_dir[512];
    if (buckets_get_data_dir(data_dir, sizeof(data_dir)) != 0) {
        /* Fallback to default */
        snprintf(data_dir, sizeof(data_dir), "/tmp/buckets-data");
    }
    snprintf(path, path_size, "%s/%s/.multipart/%s", data_dir, bucket, upload_id);
}

/**
 * Get part file path
 */
static void get_part_path(const char *bucket, const char *upload_id, int part_number, 
                          char *path, size_t path_size)
{
    /* Get data directory from storage config */
    extern int buckets_get_data_dir(char *data_dir, size_t size);
    char data_dir[512];
    if (buckets_get_data_dir(data_dir, sizeof(data_dir)) != 0) {
        /* Fallback to default */
        snprintf(data_dir, sizeof(data_dir), "/tmp/buckets-data");
    }
    snprintf(path, path_size, "%s/%s/.multipart/%s/parts/part.%d",
             data_dir, bucket, upload_id, part_number);
}

/**
 * Get metadata file path
 */
static void get_metadata_path(const char *bucket, const char *upload_id, 
                              char *path, size_t path_size)
{
    /* Get data directory from storage config */
    extern int buckets_get_data_dir(char *data_dir, size_t size);
    char data_dir[512];
    if (buckets_get_data_dir(data_dir, sizeof(data_dir)) != 0) {
        /* Fallback to default */
        snprintf(data_dir, sizeof(data_dir), "/tmp/buckets-data");
    }
    snprintf(path, path_size, "%s/%s/.multipart/%s/metadata.json",
             data_dir, bucket, upload_id);
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
    char data_dir[512];
    
    /* Get data directory */
    extern int buckets_get_data_dir(char *data_dir, size_t size);
    if (buckets_get_data_dir(data_dir, sizeof(data_dir)) != 0) {
        snprintf(data_dir, sizeof(data_dir), "/tmp/buckets-data");
    }
    
    /* Create bucket directory if needed */
    snprintf(path, sizeof(path), "%s/%s", data_dir, bucket);
    mkdir(path, 0755);  /* Ignore error if exists */
    
    /* Create .multipart directory */
    snprintf(path, sizeof(path), "%s/%s/.multipart", data_dir, bucket);
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
    snprintf(path, sizeof(path), "%s/%s/.multipart/%s/parts",
             data_dir, bucket, upload_id);
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
    
    /* Validate bucket exists (check any disk) */
    extern int buckets_get_data_dir(char *data_dir, size_t size);
    char data_dir[512];
    if (buckets_get_data_dir(data_dir, sizeof(data_dir)) != 0) {
        snprintf(data_dir, sizeof(data_dir), "/tmp/buckets-data");
    }
    
    char bucket_path[2048];
    snprintf(bucket_path, sizeof(bucket_path), "%s/disk1/%s", data_dir, req->bucket);
    
    struct stat st;
    if (stat(bucket_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        /* Try without disk subdirectory (single-disk mode) */
        snprintf(bucket_path, sizeof(bucket_path), "%s/%s", data_dir, req->bucket);
        if (stat(bucket_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return buckets_s3_xml_error(res, "NoSuchBucket",
                                         "The specified bucket does not exist",
                                         req->bucket);
        }
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
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Get uploadId from query parameters */
    const char *upload_id = NULL;
    
    for (int i = 0; i < req->query_count; i++) {
        if (strcmp(req->query_params_keys[i], "uploadId") == 0) {
            upload_id = req->query_params_values[i];
            break;
        }
    }
    
    if (!upload_id) {
        return buckets_s3_xml_error(res, "InvalidRequest",
                                     "uploadId is required",
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
    
    buckets_info("⏱️  CompleteMultipart START: bucket=%s, key=%s, uploadId=%s",
                  req->bucket, req->key, upload_id);
    
    /* Parse request body (XML with part list) */
    if (!req->body || req->body_len == 0) {
        return buckets_s3_xml_error(res, "MalformedXML",
                                     "Request body is required",
                                     req->key);
    }
    
    /* Parse XML body using cJSON (treating XML as simple structure) */
    /* Expected format:
     * <CompleteMultipartUpload>
     *   <Part><PartNumber>1</PartNumber><ETag>"etag1"</ETag></Part>
     *   <Part><PartNumber>2</PartNumber><ETag>"etag2"</ETag></Part>
     * </CompleteMultipartUpload>
     */
    
    /* For simplicity, we'll read all parts from the directory and validate they exist */
    /* In a real implementation, we'd parse the XML and validate ETags match */
    
    /* Open parts directory */
    char parts_dir[2048];
    snprintf(parts_dir, sizeof(parts_dir), "/tmp/buckets-data/%s/.multipart/%s/parts",
             req->bucket, upload_id);
    
    buckets_info("⏱️  CompleteMultipart: Opening parts directory: %s", parts_dir);
    
    DIR *dir = opendir(parts_dir);
    if (!dir) {
        buckets_error("⏱️  CompleteMultipart FAILED: Cannot open parts directory: %s (errno=%d: %s)", 
                     parts_dir, errno, strerror(errno));
        return buckets_s3_xml_error(res, "InternalError",
                                     "Failed to read parts",
                                     req->key);
    }
    
    /* Collect part numbers */
    int *part_numbers = buckets_malloc(sizeof(int) * 10000);
    if (!part_numbers) {
        closedir(dir);
        return BUCKETS_ERR_NOMEM;
    }
    int part_count = 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Parse "part.N" filename */
        if (strncmp(entry->d_name, "part.", 5) != 0) {
            continue;
        }
        
        int part_number = atoi(entry->d_name + 5);
        if (part_number < 1 || part_number > 10000) {
            continue;
        }
        
        part_numbers[part_count++] = part_number;
        
        if (part_count >= 10000) {
            break;
        }
    }
    closedir(dir);
    
    buckets_info("⏱️  CompleteMultipart: Found %d parts", part_count);
    
    if (part_count == 0) {
        buckets_error("⏱️  CompleteMultipart FAILED: No parts found");
        buckets_free(part_numbers);
        return buckets_s3_xml_error(res, "InvalidPart",
                                     "No parts found for upload",
                                     upload_id);
    }
    
    /* Sort part numbers */
    for (int i = 0; i < part_count - 1; i++) {
        for (int j = i + 1; j < part_count; j++) {
            if (part_numbers[i] > part_numbers[j]) {
                int temp = part_numbers[i];
                part_numbers[i] = part_numbers[j];
                part_numbers[j] = temp;
            }
        }
    }
    
    /* Concatenate parts into a single buffer */
    /* First, calculate total size by reading all parts */
    buckets_info("⏱️  CompleteMultipart: Calculating total size from %d parts", part_count);
    size_t total_size = 0;
    for (int i = 0; i < part_count; i++) {
        char part_path[2048];
        get_part_path(req->bucket, upload_id, part_numbers[i], part_path, sizeof(part_path));
        
        FILE *part_fp = fopen(part_path, "rb");
        if (!part_fp) {
            buckets_error("Failed to open part file: %s", part_path);
            buckets_free(part_numbers);
            return buckets_s3_xml_error(res, "InternalError",
                                         "Failed to read part",
                                         req->key);
        }
        
        /* Get part size */
        fseek(part_fp, 0, SEEK_END);
        long part_size = ftell(part_fp);
        fclose(part_fp);
        
        if (part_size < 0) {
            buckets_error("Failed to get part size: %s", part_path);
            buckets_free(part_numbers);
            return buckets_s3_xml_error(res, "InternalError",
                                         "Failed to read part size",
                                         req->key);
        }
        
        total_size += (size_t)part_size;
    }
    
    buckets_info("⏱️  CompleteMultipart: Total size = %zu bytes (%.2f MB)", 
                 total_size, total_size / 1024.0 / 1024.0);
    
    /* Allocate buffer for complete object */
    char *final_data = buckets_malloc(total_size);
    if (!final_data) {
        buckets_error("⏱️  CompleteMultipart FAILED: Cannot allocate %zu bytes", total_size);
        buckets_free(part_numbers);
        return BUCKETS_ERR_NOMEM;
    }
    
    buckets_info("⏱️  CompleteMultipart: Allocated %zu bytes, reading parts...", total_size);
    
    /* Read all parts into buffer */
    size_t offset = 0;
    for (int i = 0; i < part_count; i++) {
        char part_path[2048];
        get_part_path(req->bucket, upload_id, part_numbers[i], part_path, sizeof(part_path));
        
        FILE *part_fp = fopen(part_path, "rb");
        if (!part_fp) {
            buckets_error("Failed to open part file: %s", part_path);
            buckets_free(final_data);
            buckets_free(part_numbers);
            return buckets_s3_xml_error(res, "InternalError",
                                         "Failed to read part",
                                         req->key);
        }
        
        /* Read part data */
        size_t bytes_read = fread(final_data + offset, 1, total_size - offset, part_fp);
        int had_error = ferror(part_fp);
        fclose(part_fp);
        
        if (bytes_read == 0 && had_error) {
            buckets_error("Failed to read part data: %s", part_path);
            buckets_free(final_data);
            buckets_free(part_numbers);
            return buckets_s3_xml_error(res, "InternalError",
                                         "Failed to read part data",
                                         req->key);
        }
        
        offset += bytes_read;
    }
    
    buckets_free(part_numbers);
    
    buckets_info("⏱️  CompleteMultipart: All parts read successfully (%zu bytes total)", total_size);
    
    /* Calculate ETag before writing */
    char etag[33];
    buckets_s3_calculate_etag(final_data, total_size, etag);
    
    buckets_info("⏱️  CompleteMultipart: ETag calculated: %s", etag);
    
    /* Write object using storage layer (creates xl.meta and registers in registry) */
    extern int buckets_put_object(const char *bucket, const char *object,
                                   const void *data, size_t size,
                                   const char *content_type);
    
    buckets_info("⏱️  CompleteMultipart: Calling buckets_put_object for %zu bytes...", total_size);
    
    int ret = buckets_put_object(req->bucket, req->key, final_data, total_size, NULL);
    buckets_free(final_data);
    
    if (ret != 0) {
        buckets_error("⏱️  CompleteMultipart FAILED: buckets_put_object returned %d for %s/%s", 
                     ret, req->bucket, req->key);
        return buckets_s3_xml_error(res, "InternalError",
                                     "Failed to write completed object",
                                     req->key);
    }
    
    buckets_info("⏱️  CompleteMultipart: buckets_put_object succeeded");
    
    /* For multipart uploads, append "-{part_count}" to indicate it's a multipart object */
    char multipart_etag[48];  /* 32 (MD5) + 1 (dash) + 5 (digits) + 1 (null) = 39, round to 48 */
    snprintf(multipart_etag, sizeof(multipart_etag), "%s-%d", etag, part_count);
    
    /* Clean up multipart upload directory */
    char upload_dir[2048];
    get_multipart_dir(req->bucket, upload_id, upload_dir, sizeof(upload_dir));
    remove_directory(upload_dir);
    
    buckets_info("⏱️  CompleteMultipart SUCCESS: %s/%s (etag=%s, parts=%d, size=%zu)",
                 req->bucket, req->key, multipart_etag, part_count, total_size);
    
    /* Generate XML response */
    char xml_body[4096];
    snprintf(xml_body, sizeof(xml_body),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<CompleteMultipartUploadResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
             "  <Location>http://localhost:9000/%s/%s</Location>\n"
             "  <Bucket>%s</Bucket>\n"
             "  <Key>%s</Key>\n"
             "  <ETag>\"%s\"</ETag>\n"
             "</CompleteMultipartUploadResult>\n",
             req->bucket, req->key, req->bucket, req->key, multipart_etag);
    
    res->status_code = 200;
    res->body = buckets_strdup(xml_body);
    res->body_len = strlen(xml_body);
    snprintf(res->etag, sizeof(res->etag), "\"%s\"", multipart_etag);
    
    return BUCKETS_OK;
}

int buckets_s3_abort_multipart_upload(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Get uploadId from query parameters */
    const char *upload_id = NULL;
    
    for (int i = 0; i < req->query_count; i++) {
        if (strcmp(req->query_params_keys[i], "uploadId") == 0) {
            upload_id = req->query_params_values[i];
            break;
        }
    }
    
    if (!upload_id) {
        return buckets_s3_xml_error(res, "InvalidRequest",
                                     "uploadId is required",
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
    
    buckets_debug("Aborting multipart upload: bucket=%s, key=%s, uploadId=%s",
                  req->bucket, req->key, upload_id);
    
    /* Remove upload directory and all parts */
    char upload_dir[2048];
    get_multipart_dir(req->bucket, upload_id, upload_dir, sizeof(upload_dir));
    remove_directory(upload_dir);
    
    buckets_info("Multipart upload aborted: %s/%s (uploadId=%s)",
                 req->bucket, req->key, upload_id);
    
    /* Return 204 No Content */
    res->status_code = 204;
    res->body = NULL;
    res->body_len = 0;
    
    return BUCKETS_OK;
}

int buckets_s3_list_parts(buckets_s3_request_t *req, buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Get uploadId from query parameters */
    const char *upload_id = NULL;
    const char *max_parts_str = NULL;
    const char *part_number_marker_str = NULL;
    
    for (int i = 0; i < req->query_count; i++) {
        if (strcmp(req->query_params_keys[i], "uploadId") == 0) {
            upload_id = req->query_params_values[i];
        } else if (strcmp(req->query_params_keys[i], "max-parts") == 0) {
            max_parts_str = req->query_params_values[i];
        } else if (strcmp(req->query_params_keys[i], "part-number-marker") == 0) {
            part_number_marker_str = req->query_params_values[i];
        }
    }
    
    if (!upload_id) {
        return buckets_s3_xml_error(res, "InvalidRequest",
                                     "uploadId is required",
                                     req->key);
    }
    
    /* Parse pagination parameters */
    int max_parts = max_parts_str ? atoi(max_parts_str) : 1000;
    int part_number_marker = part_number_marker_str ? atoi(part_number_marker_str) : 0;
    
    if (max_parts < 1) max_parts = 1;
    if (max_parts > 1000) max_parts = 1000;
    
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
    
    buckets_debug("Listing parts: bucket=%s, key=%s, uploadId=%s, maxParts=%d, marker=%d",
                  req->bucket, req->key, upload_id, max_parts, part_number_marker);
    
    /* Open parts directory */
    char parts_dir[2048];
    snprintf(parts_dir, sizeof(parts_dir), "/tmp/buckets-data/%s/.multipart/%s/parts",
             req->bucket, upload_id);
    
    DIR *dir = opendir(parts_dir);
    if (!dir) {
        buckets_error("Failed to open parts directory: %s", parts_dir);
        return buckets_s3_xml_error(res, "InternalError",
                                     "Failed to read parts",
                                     req->key);
    }
    
    /* Collect part information */
    typedef struct {
        int part_number;
        size_t size;
        char etag[33];
        time_t last_modified;
    } part_info_t;
    
    part_info_t *parts = buckets_malloc(sizeof(part_info_t) * 10000);  /* Max 10000 parts */
    if (!parts) {
        closedir(dir);
        return BUCKETS_ERR_NOMEM;
    }
    int part_count = 0;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Parse "part.N" filename */
        if (strncmp(entry->d_name, "part.", 5) != 0) {
            continue;
        }
        
        int part_number = atoi(entry->d_name + 5);
        if (part_number < 1 || part_number > 10000) {
            continue;
        }
        
        /* Skip if before marker */
        if (part_number <= part_number_marker) {
            continue;
        }
        
        /* Get part file stats */
        char part_path[2048];
        get_part_path(req->bucket, upload_id, part_number, part_path, sizeof(part_path));
        
        struct stat st;
        if (stat(part_path, &st) != 0) {
            continue;
        }
        
        /* Read part data to calculate ETag */
        FILE *fp = fopen(part_path, "rb");
        if (!fp) {
            continue;
        }
        
        /* Read file and calculate MD5 */
        size_t file_size = st.st_size;
        char *part_data = buckets_malloc(file_size);
        if (!part_data) {
            fclose(fp);
            continue;
        }
        
        size_t bytes_read = fread(part_data, 1, file_size, fp);
        fclose(fp);
        
        if (bytes_read != file_size) {
            buckets_free(part_data);
            continue;
        }
        
        /* Calculate ETag */
        char etag[33];
        buckets_s3_calculate_etag(part_data, file_size, etag);
        buckets_free(part_data);
        
        /* Store part info */
        parts[part_count].part_number = part_number;
        parts[part_count].size = file_size;
        strncpy(parts[part_count].etag, etag, sizeof(parts[part_count].etag) - 1);
        parts[part_count].etag[32] = '\0';
        parts[part_count].last_modified = st.st_mtime;
        part_count++;
        
        if (part_count >= 10000) {
            break;
        }
    }
    closedir(dir);
    
    /* Sort parts by part number */
    for (int i = 0; i < part_count - 1; i++) {
        for (int j = i + 1; j < part_count; j++) {
            if (parts[i].part_number > parts[j].part_number) {
                part_info_t temp = parts[i];
                parts[i] = parts[j];
                parts[j] = temp;
            }
        }
    }
    
    /* Generate XML response */
    char *xml_body = buckets_malloc(1024 * 1024);  /* 1 MB buffer */
    if (!xml_body) {
        buckets_free(parts);
        return BUCKETS_ERR_NOMEM;
    }
    
    int offset = 0;
    offset += snprintf(xml_body + offset, 1024 * 1024 - offset,
                       "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<ListPartsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
                       "  <Bucket>%s</Bucket>\n"
                       "  <Key>%s</Key>\n"
                       "  <UploadId>%s</UploadId>\n",
                       req->bucket, req->key, upload_id);
    
    /* Add parts (respecting max_parts) */
    int parts_returned = 0;
    bool is_truncated = false;
    int next_part_number_marker = 0;
    
    for (int i = 0; i < part_count && parts_returned < max_parts; i++) {
        char timestamp[64];
        struct tm *tm_info = gmtime(&parts[i].last_modified);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000Z", tm_info);
        
        offset += snprintf(xml_body + offset, 1024 * 1024 - offset,
                           "  <Part>\n"
                           "    <PartNumber>%d</PartNumber>\n"
                           "    <LastModified>%s</LastModified>\n"
                           "    <ETag>\"%s\"</ETag>\n"
                           "    <Size>%zu</Size>\n"
                           "  </Part>\n",
                           parts[i].part_number, timestamp, parts[i].etag, parts[i].size);
        
        parts_returned++;
        next_part_number_marker = parts[i].part_number;
    }
    
    if (parts_returned < part_count) {
        is_truncated = true;
    }
    
    /* Add pagination info */
    offset += snprintf(xml_body + offset, 1024 * 1024 - offset,
                       "  <IsTruncated>%s</IsTruncated>\n",
                       is_truncated ? "true" : "false");
    
    if (is_truncated) {
        offset += snprintf(xml_body + offset, 1024 * 1024 - offset,
                           "  <NextPartNumberMarker>%d</NextPartNumberMarker>\n",
                           next_part_number_marker);
    }
    
    offset += snprintf(xml_body + offset, 1024 * 1024 - offset,
                       "  <MaxParts>%d</MaxParts>\n"
                       "</ListPartsResult>\n",
                       max_parts);
    
    buckets_free(parts);
    
    res->status_code = 200;
    res->body = xml_body;
    res->body_len = offset;
    
    buckets_info("Listed %d parts for %s/%s (uploadId=%s)",
                 parts_returned, req->bucket, req->key, upload_id);
    
    return BUCKETS_OK;
}
