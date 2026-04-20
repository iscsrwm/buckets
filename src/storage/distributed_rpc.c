/**
 * Distributed Storage RPC Handlers
 * 
 * Implements RPC methods for cross-node chunk distribution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#include "buckets.h"
#include "buckets_net.h"
#include "buckets_storage.h"
#include "cJSON.h"

/* ===================================================================
 * RPC Method: storage.writeChunk
 * 
 * Writes a chunk to local disk on behalf of remote node.
 * 
 * Request params:
 * {
 *   "bucket": "my-bucket",
 *   "object": "my-object",
 *   "chunk_index": 1,
 *   "chunk_data": "<base64-encoded-data>",
 *   "chunk_size": 131072,
 *   "disk_path": "/tmp/buckets-node2/disk1"
 * }
 * 
 * Response result:
 * {
 *   "success": true,
 *   "bytes_written": 131072
 * }
 * ===================================================================*/

/**
 * Base64 decode helper (simplified)
 */
static u8* base64_decode(const char *input, size_t *output_len)
{
    static const char base64_chars[] = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    size_t input_len = strlen(input);
    size_t decoded_len = (input_len / 4) * 3;
    
    /* Adjust for padding */
    if (input[input_len - 1] == '=') decoded_len--;
    if (input[input_len - 2] == '=') decoded_len--;
    
    u8 *decoded = buckets_malloc(decoded_len + 1);
    size_t j = 0;
    u32 buffer = 0;
    int bits = 0;
    
    for (size_t i = 0; i < input_len; i++) {
        if (input[i] == '=') break;
        
        const char *p = strchr(base64_chars, input[i]);
        if (!p) continue;
        
        buffer = (buffer << 6) | (p - base64_chars);
        bits += 6;
        
        if (bits >= 8) {
            bits -= 8;
            decoded[j++] = (buffer >> bits) & 0xFF;
        }
    }
    
    *output_len = j;
    return decoded;
}

/**
 * Base64 encode helper
 */
static char* base64_encode(const u8 *data, size_t len)
{
    static const char base64_chars[] = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    size_t encoded_len = ((len + 2) / 3) * 4;
    char *encoded = buckets_malloc(encoded_len + 1);
    
    size_t i = 0, j = 0;
    while (i < len) {
        u32 octet_a = i < len ? data[i++] : 0;
        u32 octet_b = i < len ? data[i++] : 0;
        u32 octet_c = i < len ? data[i++] : 0;
        
        u32 triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        
        encoded[j++] = base64_chars[(triple >> 18) & 0x3F];
        encoded[j++] = base64_chars[(triple >> 12) & 0x3F];
        encoded[j++] = base64_chars[(triple >> 6) & 0x3F];
        encoded[j++] = base64_chars[triple & 0x3F];
    }
    
    /* Add padding */
    int padding = (3 - (len % 3)) % 3;
    for (int p = 0; p < padding; p++) {
        encoded[encoded_len - 1 - p] = '=';
    }
    
    encoded[encoded_len] = '\0';
    return encoded;
}

/**
 * RPC handler: storage.writeChunk
 */
static int rpc_handler_write_chunk(const char *method,
                                    cJSON *params,
                                    cJSON **result,
                                    int *error_code,
                                    char *error_message,
                                    void *user_data)
{
    (void)method;
    (void)user_data;
    
    *error_code = 0;
    error_message[0] = '\0';
    
    /* Parse parameters */
    cJSON *bucket_json = cJSON_GetObjectItem(params, "bucket");
    cJSON *object_json = cJSON_GetObjectItem(params, "object");
    cJSON *chunk_index_json = cJSON_GetObjectItem(params, "chunk_index");
    cJSON *chunk_data_json = cJSON_GetObjectItem(params, "chunk_data");
    cJSON *chunk_size_json = cJSON_GetObjectItem(params, "chunk_size");
    cJSON *disk_path_json = cJSON_GetObjectItem(params, "disk_path");
    
    if (!bucket_json || !object_json || !chunk_index_json || 
        !chunk_data_json || !chunk_size_json || !disk_path_json) {
        *error_code = -1;
        snprintf(error_message, 256, "Missing required parameters");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    const char *bucket = bucket_json->valuestring;
    const char *object = object_json->valuestring;
    u32 chunk_index = (u32)chunk_index_json->valueint;
    const char *chunk_data_b64 = chunk_data_json->valuestring;
    size_t chunk_size = (size_t)chunk_size_json->valueint;
    const char *disk_path = disk_path_json->valuestring;
    
    /* Decode base64 chunk data */
    size_t decoded_len = 0;
    u8 *chunk_data = base64_decode(chunk_data_b64, &decoded_len);
    
    if (decoded_len != chunk_size) {
        buckets_free(chunk_data);
        *error_code = -1;
        snprintf(error_message, 256, "Chunk size mismatch: expected %zu, got %zu",
                 chunk_size, decoded_len);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Compute object path */
    char object_path[PATH_MAX];
    extern void buckets_compute_object_path(const char *bucket, const char *object,
                                            char *path, size_t path_len);
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Create object directory if needed */
    extern int buckets_create_object_dir(const char *disk_path, const char *object_path);
    if (buckets_create_object_dir(disk_path, object_path) != 0) {
        buckets_free(chunk_data);
        *error_code = -1;
        snprintf(error_message, 256, "Failed to create object directory");
        return BUCKETS_ERR_IO;
    }
    
    /* Write chunk */
    int ret = buckets_write_chunk(disk_path, object_path, chunk_index, 
                                   chunk_data, chunk_size);
    
    buckets_free(chunk_data);
    
    if (ret != 0) {
        *error_code = -1;
        snprintf(error_message, 256, "Failed to write chunk %d", chunk_index);
        return BUCKETS_ERR_IO;
    }
    
    /* Build success response */
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "success", true);
    cJSON_AddNumberToObject(*result, "bytes_written", (double)chunk_size);
    
    buckets_debug("RPC writeChunk: %s/%s chunk %d (%zu bytes) -> %s",
                  bucket, object, chunk_index, chunk_size, disk_path);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * RPC Method: storage.readChunk
 * 
 * Reads a chunk from local disk on behalf of remote node.
 * 
 * Request params:
 * {
 *   "bucket": "my-bucket",
 *   "object": "my-object",
 *   "chunk_index": 1,
 *   "disk_path": "/tmp/buckets-node2/disk1"
 * }
 * 
 * Response result:
 * {
 *   "success": true,
 *   "chunk_data": "<base64-encoded-data>",
 *   "chunk_size": 131072
 * }
 * ===================================================================*/

/**
 * RPC handler: storage.readChunk
 */
static int rpc_handler_read_chunk(const char *method,
                                   cJSON *params,
                                   cJSON **result,
                                   int *error_code,
                                   char *error_message,
                                   void *user_data)
{
    (void)method;
    (void)user_data;
    
    *error_code = 0;
    error_message[0] = '\0';
    
    /* Parse parameters */
    cJSON *bucket_json = cJSON_GetObjectItem(params, "bucket");
    cJSON *object_json = cJSON_GetObjectItem(params, "object");
    cJSON *chunk_index_json = cJSON_GetObjectItem(params, "chunk_index");
    cJSON *disk_path_json = cJSON_GetObjectItem(params, "disk_path");
    
    if (!bucket_json || !object_json || !chunk_index_json || !disk_path_json) {
        *error_code = -1;
        snprintf(error_message, 256, "Missing required parameters");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    const char *bucket = bucket_json->valuestring;
    const char *object = object_json->valuestring;
    u32 chunk_index = (u32)chunk_index_json->valueint;
    const char *disk_path = disk_path_json->valuestring;
    
    /* Compute object path */
    char object_path[PATH_MAX];
    extern void buckets_compute_object_path(const char *bucket, const char *object,
                                            char *path, size_t path_len);
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Read chunk */
    void *chunk_data = NULL;
    size_t chunk_size = 0;
    
    int ret = buckets_read_chunk(disk_path, object_path, chunk_index, 
                                 &chunk_data, &chunk_size);
    
    if (ret != 0 || !chunk_data) {
        *error_code = -1;
        snprintf(error_message, 256, "Failed to read chunk %d", chunk_index);
        return BUCKETS_ERR_IO;
    }
    
    /* Encode to base64 */
    char *chunk_data_b64 = base64_encode((const u8*)chunk_data, chunk_size);
    buckets_free(chunk_data);
    
    /* Build success response */
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "success", true);
    cJSON_AddStringToObject(*result, "chunk_data", chunk_data_b64);
    cJSON_AddNumberToObject(*result, "chunk_size", (double)chunk_size);
    
    buckets_free(chunk_data_b64);
    
    buckets_debug("RPC readChunk: %s/%s chunk %d (%zu bytes) from %s",
                  bucket, object, chunk_index, chunk_size, disk_path);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * RPC Method: storage.writeXlMeta
 * 
 * Writes xl.meta file to local disk on behalf of remote node.
 * 
 * Request params:
 * {
 *   "bucket": "my-bucket",
 *   "object": "my-object",
 *   "disk_path": "/tmp/buckets-node2/disk1",
 *   "xl_meta_json": "<json-encoded-xl-meta>"
 * }
 * 
 * Response result:
 * {
 *   "success": true
 * }
 * ===================================================================*/

/**
 * RPC handler: storage.writeXlMeta
 */
static int rpc_handler_write_xlmeta(const char *method,
                                     cJSON *params,
                                     cJSON **result,
                                     int *error_code,
                                     char *error_message,
                                     void *user_data)
{
    (void)method;
    (void)user_data;
    
    *error_code = 0;
    error_message[0] = '\0';
    
    /* Parse parameters */
    cJSON *bucket_json = cJSON_GetObjectItem(params, "bucket");
    cJSON *object_json = cJSON_GetObjectItem(params, "object");
    cJSON *disk_path_json = cJSON_GetObjectItem(params, "disk_path");
    cJSON *xl_meta_json = cJSON_GetObjectItem(params, "xl_meta_json");
    
    if (!bucket_json || !object_json || !disk_path_json || !xl_meta_json) {
        *error_code = -1;
        snprintf(error_message, 256, "Missing required parameters");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    const char *bucket = bucket_json->valuestring;
    const char *object = object_json->valuestring;
    const char *disk_path = disk_path_json->valuestring;
    const char *meta_json_str = xl_meta_json->valuestring;
    
    /* Compute object path */
    char object_path[PATH_MAX];
    extern void buckets_compute_object_path(const char *bucket, const char *object,
                                            char *path, size_t path_len);
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Parse xl.meta from JSON */
    extern int buckets_xl_meta_from_json(const char *json_str, buckets_xl_meta_t *meta);
    extern int buckets_write_xl_meta(const char *disk_path, const char *object_path,
                                     const buckets_xl_meta_t *meta);
    extern void buckets_xl_meta_free(buckets_xl_meta_t *meta);
    
    buckets_xl_meta_t meta;
    int ret = buckets_xl_meta_from_json(meta_json_str, &meta);
    
    if (ret != 0) {
        *error_code = -1;
        snprintf(error_message, 256, "Failed to parse xl.meta JSON");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Create object directory if needed */
    extern int buckets_create_object_dir(const char *disk_path, const char *object_path);
    if (buckets_create_object_dir(disk_path, object_path) != 0) {
        buckets_xl_meta_free(&meta);
        *error_code = -1;
        snprintf(error_message, 256, "Failed to create object directory");
        return BUCKETS_ERR_IO;
    }
    
    /* Write xl.meta */
    ret = buckets_write_xl_meta(disk_path, object_path, &meta);
    buckets_xl_meta_free(&meta);
    
    if (ret != 0) {
        *error_code = -1;
        snprintf(error_message, 256, "Failed to write xl.meta");
        return BUCKETS_ERR_IO;
    }
    
    /* Build success response */
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "success", true);
    
    buckets_debug("RPC writeXlMeta: %s/%s to %s",
                  bucket, object, disk_path);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * RPC Method: storage.readXlMeta
 * 
 * Reads xl.meta file from local disk on behalf of remote node.
 * 
 * Request params:
 * {
 *   "bucket": "my-bucket",
 *   "object": "my-object",
 *   "disk_path": "/tmp/buckets-node2/disk1"
 * }
 * 
 * Response result:
 * {
 *   "success": true,
 *   "xl_meta_json": "<json-encoded-xl-meta>"
 * }
 * ===================================================================*/

/**
 * RPC handler: storage.readXlMeta
 */
static int rpc_handler_read_xlmeta(const char *method,
                                    cJSON *params,
                                    cJSON **result,
                                    int *error_code,
                                    char *error_message,
                                    void *user_data)
{
    (void)method;
    (void)user_data;
    
    *error_code = 0;
    error_message[0] = '\0';
    
    /* Parse parameters */
    cJSON *bucket_json = cJSON_GetObjectItem(params, "bucket");
    cJSON *object_json = cJSON_GetObjectItem(params, "object");
    cJSON *disk_path_json = cJSON_GetObjectItem(params, "disk_path");
    
    if (!bucket_json || !object_json || !disk_path_json) {
        *error_code = -1;
        snprintf(error_message, 256, "Missing required parameters");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    const char *bucket = bucket_json->valuestring;
    const char *object = object_json->valuestring;
    const char *disk_path = disk_path_json->valuestring;
    
    /* Compute object path */
    char object_path[PATH_MAX];
    extern void buckets_compute_object_path(const char *bucket, const char *object,
                                            char *path, size_t path_len);
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Read xl.meta */
    extern int buckets_read_xl_meta(const char *disk_path, const char *object_path,
                                    buckets_xl_meta_t *meta);
    extern char* buckets_xl_meta_to_json(const buckets_xl_meta_t *meta);
    
    buckets_xl_meta_t meta;
    int ret = buckets_read_xl_meta(disk_path, object_path, &meta);
    
    if (ret != 0) {
        *error_code = -1;
        snprintf(error_message, 256, "Failed to read xl.meta");
        return BUCKETS_ERR_IO;
    }
    
    /* Convert xl.meta to JSON string */
    extern char* buckets_xl_meta_to_json(const buckets_xl_meta_t *meta);
    char *meta_json = buckets_xl_meta_to_json(&meta);
    
    buckets_xl_meta_free(&meta);
    
    if (!meta_json) {
        *error_code = -1;
        snprintf(error_message, 256, "Failed to serialize xl.meta to JSON");
        return BUCKETS_ERR_IO;
    }
    
    /* Build success response */
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "success", true);
    cJSON_AddStringToObject(*result, "xl_meta_json", meta_json);
    
    buckets_free(meta_json);
    
    buckets_debug("RPC readXlMeta: %s/%s from %s",
                  bucket, object, disk_path);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * RPC Method: storage.deleteChunk
 * 
 * Deletes a chunk and xl.meta from local disk on behalf of remote node.
 * 
 * Request params:
 * {
 *   "bucket": "my-bucket",
 *   "object": "my-object",
 *   "chunk_index": 1,
 *   "disk_path": "/tmp/buckets-node2/disk1"
 * }
 * 
 * Response result:
 * {
 *   "success": true
 * }
 * ===================================================================*/

static int rpc_handler_delete_chunk(const char *method, cJSON *params, cJSON **result,
                                     int *error_code, char *error_message,
                                     void *user_data)
{
    (void)method;
    (void)user_data;
    
    if (!params || !result || !error_code || !error_message) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Extract params */
    cJSON *bucket_json = cJSON_GetObjectItem(params, "bucket");
    cJSON *object_json = cJSON_GetObjectItem(params, "object");
    cJSON *chunk_index_json = cJSON_GetObjectItem(params, "chunk_index");
    cJSON *disk_path_json = cJSON_GetObjectItem(params, "disk_path");
    
    if (!bucket_json || !cJSON_IsString(bucket_json) ||
        !object_json || !cJSON_IsString(object_json) ||
        !chunk_index_json || !cJSON_IsNumber(chunk_index_json) ||
        !disk_path_json || !cJSON_IsString(disk_path_json)) {
        *error_code = -1;
        snprintf(error_message, 256, "Missing required params");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    const char *bucket = bucket_json->valuestring;
    const char *object = object_json->valuestring;
    u32 chunk_index = (u32)chunk_index_json->valueint;
    const char *disk_path = disk_path_json->valuestring;
    
    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    int deleted = 0;
    
    /* Delete chunk file */
    char chunk_path[PATH_MAX * 2];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%spart.%u", disk_path, object_path, chunk_index);
    if (unlink(chunk_path) == 0) {
        buckets_debug("RPC deleteChunk: deleted chunk %s", chunk_path);
        deleted++;
    }
    
    /* Delete xl.meta */
    char meta_path[PATH_MAX * 2];
    snprintf(meta_path, sizeof(meta_path), "%s/%sxl.meta", disk_path, object_path);
    if (unlink(meta_path) == 0) {
        buckets_debug("RPC deleteChunk: deleted xl.meta %s", meta_path);
        deleted++;
    }
    
    /* Try to remove object directory */
    char dir_path[PATH_MAX * 2];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", disk_path, object_path);
    size_t len = strlen(dir_path);
    if (len > 0 && dir_path[len-1] == '/') {
        dir_path[len-1] = '\0';
    }
    rmdir(dir_path);
    
    /* Build success response */
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "success", deleted > 0);
    
    buckets_debug("RPC deleteChunk: %s/%s chunk %u from %s - deleted=%d",
                  bucket, object, chunk_index, disk_path, deleted);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * RPC Method: storage.createBucketDir
 * 
 * Creates bucket directory on local disks.
 * Called when creating a bucket in distributed mode.
 * 
 * Request params:
 * {
 *   "bucket": "my-bucket"
 * }
 * 
 * Response result:
 * {
 *   "success": true,
 *   "disks_created": 4
 * }
 * ===================================================================*/

static int rpc_handler_create_bucket_dir(const char *method, cJSON *params, cJSON **result,
                                         int *error_code, char *error_message,
                                         void *user_data)
{
    (void)method;
    (void)user_data;
    
    *error_code = 0;
    error_message[0] = '\0';
    
    /* Parse parameters */
    cJSON *bucket_json = cJSON_GetObjectItem(params, "bucket");
    
    if (!bucket_json || !cJSON_IsString(bucket_json)) {
        *error_code = -1;
        snprintf(error_message, 256, "Missing required parameter: bucket");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    const char *bucket = bucket_json->valuestring;
    
    /* Get storage data directory */
    extern const buckets_storage_config_t* buckets_storage_get_config(void);
    const buckets_storage_config_t *storage_config = buckets_storage_get_config();
    if (!storage_config || !storage_config->data_dir) {
        *error_code = -1;
        snprintf(error_message, 256, "Storage not configured");
        return BUCKETS_ERR_INIT;
    }
    const char *base_dir = storage_config->data_dir;
    
    /* Create bucket directory on all local disks */
    int disks_created = 0;
    for (int disk_num = 1; disk_num <= 4; disk_num++) {
        char bucket_path[PATH_MAX];
        snprintf(bucket_path, sizeof(bucket_path), "%s/disk%d/%s", base_dir, disk_num, bucket);
        
        struct stat st;
        if (stat(bucket_path, &st) == 0) {
            /* Directory already exists */
            disks_created++;
            continue;
        }
        
        if (mkdir(bucket_path, 0755) == 0) {
            disks_created++;
            buckets_debug("Created bucket dir: %s", bucket_path);
        } else if (errno == EEXIST) {
            disks_created++;
        } else {
            buckets_warn("Failed to create bucket dir: %s (%s)", bucket_path, strerror(errno));
        }
    }
    
    /* Create result */
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "success", disks_created > 0);
    cJSON_AddNumberToObject(*result, "disks_created", disks_created);
    
    buckets_info("RPC createBucketDir: bucket=%s, disks_created=%d", bucket, disks_created);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * RPC Method: storage.listObjects
 * 
 * Lists objects from local disks matching bucket/prefix.
 * Returns objects stored on THIS node only.
 * 
 * Request params:
 * {
 *   "bucket": "my-bucket",
 *   "prefix": "folder/",    (optional)
 *   "max_keys": 1000        (optional, default 1000)
 * }
 * 
 * Response result:
 * {
 *   "success": true,
 *   "objects": [
 *     {"bucket": "my-bucket", "object": "file1.txt", "size": 1234, "mod_time": "2026-03-06T12:00:00Z"},
 *     ...
 *   ]
 * }
 * ===================================================================*/

static int rpc_handler_list_objects(const char *method, cJSON *params, cJSON **result,
                                     int *error_code, char *error_message,
                                     void *user_data)
{
    (void)method;
    (void)user_data;
    
    *error_code = 0;
    error_message[0] = '\0';
    
    /* Parse parameters */
    cJSON *bucket_json = cJSON_GetObjectItem(params, "bucket");
    cJSON *prefix_json = cJSON_GetObjectItem(params, "prefix");
    cJSON *max_keys_json = cJSON_GetObjectItem(params, "max_keys");
    
    if (!bucket_json || !cJSON_IsString(bucket_json)) {
        *error_code = -1;
        snprintf(error_message, 256, "Missing required parameter: bucket");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    const char *bucket = bucket_json->valuestring;
    const char *prefix = (prefix_json && cJSON_IsString(prefix_json)) ? prefix_json->valuestring : NULL;
    int max_keys = (max_keys_json && cJSON_IsNumber(max_keys_json)) ? max_keys_json->valueint : 1000;
    
    /* Get storage data directory */
    extern const buckets_storage_config_t* buckets_storage_get_config(void);
    const buckets_storage_config_t *storage_config = buckets_storage_get_config();
    if (!storage_config || !storage_config->data_dir) {
        *error_code = -1;
        snprintf(error_message, 256, "Storage not configured");
        return BUCKETS_ERR_INIT;
    }
    const char *base_dir = storage_config->data_dir;
    
    /* Create result array */
    cJSON *objects_array = cJSON_CreateArray();
    int found = 0;
    
    /* Scan all local disks (disk1, disk2, disk3, disk4) */
    for (int disk_num = 1; disk_num <= 4 && found < max_keys; disk_num++) {
        char disk_path[PATH_MAX];
        snprintf(disk_path, sizeof(disk_path), "%s/disk%d", base_dir, disk_num);
        
        DIR *disk_dir = opendir(disk_path);
        if (!disk_dir) continue;
        
        /* Scan hash prefix directories (00-ff) */
        struct dirent *prefix_entry;
        while ((prefix_entry = readdir(disk_dir)) != NULL && found < max_keys) {
            if (prefix_entry->d_name[0] == '.') continue;
            if (strlen(prefix_entry->d_name) != 2) continue;
            
            /* Check if it's a hex prefix */
            char c1 = prefix_entry->d_name[0];
            char c2 = prefix_entry->d_name[1];
            bool is_hex = ((c1 >= '0' && c1 <= '9') || (c1 >= 'a' && c1 <= 'f')) &&
                          ((c2 >= '0' && c2 <= '9') || (c2 >= 'a' && c2 <= 'f'));
            if (!is_hex) continue;
            
            /* Scan hash directories */
            char hash_prefix_path[PATH_MAX];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
            snprintf(hash_prefix_path, sizeof(hash_prefix_path), "%s/%s", 
                     disk_path, prefix_entry->d_name);
#pragma GCC diagnostic pop
            
            DIR *hash_dir = opendir(hash_prefix_path);
            if (!hash_dir) continue;
            
            struct dirent *hash_entry;
            while ((hash_entry = readdir(hash_dir)) != NULL && found < max_keys) {
                if (hash_entry->d_name[0] == '.') continue;
                
                /* Read xl.meta */
                char meta_path[PATH_MAX * 2];
                snprintf(meta_path, sizeof(meta_path), "%s/%s/xl.meta", 
                         hash_prefix_path, hash_entry->d_name);
                
                FILE *f = fopen(meta_path, "r");
                if (!f) continue;
                
                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fseek(f, 0, SEEK_SET);
                
                if (file_size <= 0 || file_size > 1024 * 1024) {
                    fclose(f);
                    continue;
                }
                
                char *meta_content = buckets_malloc(file_size + 1);
                if (!meta_content) {
                    fclose(f);
                    continue;
                }
                
                size_t read_size = fread(meta_content, 1, file_size, f);
                fclose(f);
                meta_content[read_size] = '\0';
                
                /* Parse xl.meta JSON */
                cJSON *meta_json = cJSON_Parse(meta_content);
                buckets_free(meta_content);
                if (!meta_json) continue;
                
                /* Get bucket and object from xl.meta */
                cJSON *meta_bucket = cJSON_GetObjectItem(meta_json, "bucket");
                cJSON *meta_object = cJSON_GetObjectItem(meta_json, "object");
                
                if (!meta_bucket || !cJSON_IsString(meta_bucket) ||
                    !meta_object || !cJSON_IsString(meta_object)) {
                    cJSON_Delete(meta_json);
                    continue;
                }
                
                /* Check bucket match */
                if (strcmp(meta_bucket->valuestring, bucket) != 0) {
                    cJSON_Delete(meta_json);
                    continue;
                }
                
                /* Skip internal buckets */
                if (meta_bucket->valuestring[0] == '.') {
                    cJSON_Delete(meta_json);
                    continue;
                }
                
                /* Check prefix match */
                if (prefix && strncmp(meta_object->valuestring, prefix, strlen(prefix)) != 0) {
                    cJSON_Delete(meta_json);
                    continue;
                }
                
                /* Get size and mod_time from stat */
                size_t obj_size = 0;
                const char *mod_time = "";
                cJSON *stat = cJSON_GetObjectItem(meta_json, "stat");
                if (stat) {
                    cJSON *size_json = cJSON_GetObjectItem(stat, "size");
                    if (size_json && cJSON_IsNumber(size_json)) {
                        obj_size = (size_t)size_json->valuedouble;
                    }
                    cJSON *mod_time_json = cJSON_GetObjectItem(stat, "modTime");
                    if (mod_time_json && cJSON_IsString(mod_time_json)) {
                        mod_time = mod_time_json->valuestring;
                    }
                }
                
                /* Add to results */
                cJSON *obj_entry = cJSON_CreateObject();
                cJSON_AddStringToObject(obj_entry, "bucket", meta_bucket->valuestring);
                cJSON_AddStringToObject(obj_entry, "object", meta_object->valuestring);
                cJSON_AddNumberToObject(obj_entry, "size", (double)obj_size);
                cJSON_AddStringToObject(obj_entry, "mod_time", mod_time);
                cJSON_AddItemToArray(objects_array, obj_entry);
                found++;
                
                cJSON_Delete(meta_json);
            }
            
            closedir(hash_dir);
        }
        
        closedir(disk_dir);
    }
    
    /* Build success response */
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "success", true);
    cJSON_AddItemToObject(*result, "objects", objects_array);
    cJSON_AddNumberToObject(*result, "count", found);
    
    buckets_debug("RPC listObjects: bucket=%s prefix=%s found=%d",
                  bucket, prefix ? prefix : "(none)", found);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Initialization
 * ===================================================================*/

/**
 * Register distributed storage RPC handlers
 */
int buckets_distributed_rpc_init(buckets_rpc_context_t *rpc_ctx)
{
    if (!rpc_ctx) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Register writeChunk handler */
    int ret = buckets_rpc_register_handler(rpc_ctx, "storage.writeChunk",
                                           rpc_handler_write_chunk, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register storage.writeChunk handler");
        return ret;
    }
    
    /* Register readChunk handler */
    ret = buckets_rpc_register_handler(rpc_ctx, "storage.readChunk",
                                       rpc_handler_read_chunk, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register storage.readChunk handler");
        return ret;
    }
    
    /* Register writeXlMeta handler */
    ret = buckets_rpc_register_handler(rpc_ctx, "storage.writeXlMeta",
                                       rpc_handler_write_xlmeta, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register storage.writeXlMeta handler");
        return ret;
    }
    
    /* Register readXlMeta handler */
    ret = buckets_rpc_register_handler(rpc_ctx, "storage.readXlMeta",
                                       rpc_handler_read_xlmeta, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register storage.readXlMeta handler");
        return ret;
    }
    
    /* Register deleteChunk handler */
    ret = buckets_rpc_register_handler(rpc_ctx, "storage.deleteChunk",
                                       rpc_handler_delete_chunk, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register storage.deleteChunk handler");
        return ret;
    }
    
    /* Register listObjects handler */
    ret = buckets_rpc_register_handler(rpc_ctx, "storage.listObjects",
                                       rpc_handler_list_objects, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register storage.listObjects handler");
        return ret;
    }
    
    /* Register createBucketDir handler */
    ret = buckets_rpc_register_handler(rpc_ctx, "storage.createBucketDir",
                                       rpc_handler_create_bucket_dir, NULL);
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to register storage.createBucketDir handler");
        return ret;
    }
    
    buckets_info("Distributed storage RPC handlers registered");
    return BUCKETS_OK;
}
