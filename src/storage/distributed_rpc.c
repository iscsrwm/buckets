/**
 * Distributed Storage RPC Handlers
 * 
 * Implements RPC methods for cross-node chunk distribution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    
    buckets_info("Distributed storage RPC handlers registered");
    return BUCKETS_OK;
}
