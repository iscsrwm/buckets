/**
 * Distributed Storage Operations
 * 
 * High-level functions for distributing chunks across multiple nodes via RPC.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_net.h"
#include "buckets_storage.h"
#include "buckets_cluster.h"
#include "buckets_registry.h"
#include "buckets_placement.h"
#include "cJSON.h"

/* Global RPC context for distributed operations */
static buckets_rpc_context_t *g_rpc_ctx = NULL;
static buckets_conn_pool_t *g_conn_pool = NULL;

/* Current node's endpoint (for determining local vs remote) */
static char g_local_node_endpoint[256] = {0};

/* ===================================================================
 * Initialization
 * ===================================================================*/

/**
 * Initialize distributed storage system
 */
int buckets_distributed_storage_init(void)
{
    /* Create connection pool for RPC calls */
    g_conn_pool = buckets_conn_pool_create(30);  /* max=30 connections */
    if (!g_conn_pool) {
        buckets_error("Failed to create connection pool for distributed storage");
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Create RPC context */
    g_rpc_ctx = buckets_rpc_context_create(g_conn_pool);
    if (!g_rpc_ctx) {
        buckets_error("Failed to create RPC context for distributed storage");
        buckets_conn_pool_free(g_conn_pool);
        g_conn_pool = NULL;
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Register distributed storage RPC handlers */
    if (buckets_distributed_rpc_init(g_rpc_ctx) != BUCKETS_OK) {
        buckets_error("Failed to register distributed storage RPC handlers");
        buckets_rpc_context_free(g_rpc_ctx);
        buckets_conn_pool_free(g_conn_pool);
        g_rpc_ctx = NULL;
        g_conn_pool = NULL;
        return BUCKETS_ERR_INIT;
    }
    
    buckets_info("Distributed storage initialized");
    return BUCKETS_OK;
}

/**
 * Cleanup distributed storage system
 */
void buckets_distributed_storage_cleanup(void)
{
    if (g_rpc_ctx) {
        buckets_rpc_context_free(g_rpc_ctx);
        g_rpc_ctx = NULL;
    }
    
    if (g_conn_pool) {
        buckets_conn_pool_free(g_conn_pool);
        g_conn_pool = NULL;
    }
    
    buckets_info("Distributed storage cleaned up");
}

/**
 * Get RPC context (for use by other modules)
 */
buckets_rpc_context_t* buckets_distributed_storage_get_rpc_context(void)
{
    return g_rpc_ctx;
}

/**
 * Set current node's endpoint
 * 
 * Used to determine if a disk is local or remote.
 * 
 * @param node_endpoint Current node's endpoint (e.g., "http://localhost:9001")
 * @return 0 on success, error code otherwise
 */
int buckets_distributed_set_local_endpoint(const char *node_endpoint)
{
    if (!node_endpoint) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    strncpy(g_local_node_endpoint, node_endpoint, sizeof(g_local_node_endpoint) - 1);
    g_local_node_endpoint[sizeof(g_local_node_endpoint) - 1] = '\0';
    
    buckets_info("Local node endpoint set to: %s", g_local_node_endpoint);
    
    return BUCKETS_OK;
}

/**
 * Extract node endpoint from full disk endpoint
 * 
 * Converts "http://node1:9001/mnt/disk1" to "http://node1:9001"
 * 
 * @param disk_endpoint Full disk endpoint
 * @param node_endpoint Output buffer for node endpoint (min 256 bytes)
 * @return 0 on success, error code otherwise
 */
int buckets_distributed_extract_node_endpoint(const char *disk_endpoint, char *node_endpoint, size_t size)
{
    if (!disk_endpoint || !node_endpoint || size == 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Format: http://host:port/path or https://host:port/path */
    const char *scheme_end = strstr(disk_endpoint, "://");
    if (!scheme_end) {
        /* No scheme - assume it's just a path, return empty */
        node_endpoint[0] = '\0';
        return BUCKETS_OK;
    }
    
    /* Find the end of host:port (the slash before the path) */
    scheme_end += 3;  /* Skip "://" */
    const char *path_start = strchr(scheme_end, '/');
    
    size_t endpoint_len;
    if (path_start) {
        /* Extract everything before the path */
        endpoint_len = path_start - disk_endpoint;
    } else {
        /* No path, use entire string */
        endpoint_len = strlen(disk_endpoint);
    }
    
    if (endpoint_len >= size) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    strncpy(node_endpoint, disk_endpoint, endpoint_len);
    node_endpoint[endpoint_len] = '\0';
    
    return BUCKETS_OK;
}

/**
 * Check if a disk endpoint is local to this node
 * 
 * @param disk_endpoint Full disk endpoint
 * @return true if local, false if remote
 */
bool buckets_distributed_is_local_disk(const char *disk_endpoint)
{
    if (!disk_endpoint || g_local_node_endpoint[0] == '\0') {
        /* If no local endpoint set, assume everything is local (single-node mode) */
        return true;
    }
    
    char node_endpoint[256];
    if (buckets_distributed_extract_node_endpoint(disk_endpoint, node_endpoint, sizeof(node_endpoint)) != BUCKETS_OK) {
        /* Extraction failed, assume local */
        return true;
    }
    
    if (node_endpoint[0] == '\0') {
        /* No endpoint extracted (just a path), assume local */
        return true;
    }
    
    /* Compare with local endpoint */
    return (strcmp(node_endpoint, g_local_node_endpoint) == 0);
}

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

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
 * Base64 decode helper
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

/* ===================================================================
 * Distributed Chunk Operations
 * ===================================================================*/

/**
 * Write chunk to remote node via RPC
 * 
 * @param peer_endpoint Remote node endpoint (e.g., "http://localhost:9002")
 * @param bucket Bucket name
 * @param object Object key
 * @param chunk_index Chunk index (1-based)
 * @param chunk_data Chunk data
 * @param chunk_size Chunk size
 * @param disk_path Disk path on remote node
 * @return BUCKETS_OK on success
 */
int buckets_distributed_write_chunk(const char *peer_endpoint,
                                     const char *bucket,
                                     const char *object,
                                     u32 chunk_index,
                                     const void *chunk_data,
                                     size_t chunk_size,
                                     const char *disk_path)
{
    if (!g_rpc_ctx) {
        buckets_error("Distributed storage not initialized");
        return BUCKETS_ERR_INIT;
    }
    
    if (!peer_endpoint || !bucket || !object || !chunk_data || !disk_path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Encode chunk data to base64 */
    char *chunk_data_b64 = base64_encode((const u8*)chunk_data, chunk_size);
    
    /* Build RPC parameters */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "bucket", bucket);
    cJSON_AddStringToObject(params, "object", object);
    cJSON_AddNumberToObject(params, "chunk_index", (double)chunk_index);
    cJSON_AddStringToObject(params, "chunk_data", chunk_data_b64);
    cJSON_AddNumberToObject(params, "chunk_size", (double)chunk_size);
    cJSON_AddStringToObject(params, "disk_path", disk_path);
    
    buckets_free(chunk_data_b64);
    
    /* Call RPC - use longer timeout for large chunk transfers */
    buckets_rpc_response_t *response = NULL;
    int ret = buckets_rpc_call(g_rpc_ctx, peer_endpoint, "storage.writeChunk",
                               params, &response, 300000);  /* 300 second (5 min) timeout for large chunks */
    
    cJSON_Delete(params);
    
    if (ret != BUCKETS_OK || !response) {
        buckets_error("RPC call to %s failed: storage.writeChunk", peer_endpoint);
        if (response) {
            buckets_rpc_response_free(response);
        }
        return BUCKETS_ERR_RPC;
    }
    
    /* Check response */
    if (response->error_code != 0) {
        buckets_error("Remote writeChunk failed: %s", 
                     response->error_message ? response->error_message : "unknown error");
        buckets_rpc_response_free(response);
        return BUCKETS_ERR_RPC;
    }
    
    buckets_rpc_response_free(response);
    
    buckets_debug("Distributed write: chunk %u to %s:%s (size=%zu)",
                  chunk_index, peer_endpoint, disk_path, chunk_size);
    
    return BUCKETS_OK;
}

/**
 * Read chunk from remote node via RPC
 * 
 * @param peer_endpoint Remote node endpoint
 * @param bucket Bucket name
 * @param object Object key
 * @param chunk_index Chunk index (1-based)
 * @param chunk_data Output: chunk data (caller must free)
 * @param chunk_size Output: chunk size
 * @param disk_path Disk path on remote node
 * @return BUCKETS_OK on success
 */
int buckets_distributed_read_chunk(const char *peer_endpoint,
                                    const char *bucket,
                                    const char *object,
                                    u32 chunk_index,
                                    void **chunk_data,
                                    size_t *chunk_size,
                                    const char *disk_path)
{
    if (!g_rpc_ctx) {
        buckets_error("Distributed storage not initialized");
        return BUCKETS_ERR_INIT;
    }
    
    if (!peer_endpoint || !bucket || !object || !chunk_data || !chunk_size || !disk_path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Build RPC parameters */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "bucket", bucket);
    cJSON_AddStringToObject(params, "object", object);
    cJSON_AddNumberToObject(params, "chunk_index", (double)chunk_index);
    cJSON_AddStringToObject(params, "disk_path", disk_path);
    
    /* Call RPC */
    buckets_rpc_response_t *response = NULL;
    int ret = buckets_rpc_call(g_rpc_ctx, peer_endpoint, "storage.readChunk",
                               params, &response, 30000);  /* 30 second timeout */
    
    cJSON_Delete(params);
    
    if (ret != BUCKETS_OK || !response) {
        buckets_error("RPC call to %s failed: storage.readChunk", peer_endpoint);
        if (response) {
            buckets_rpc_response_free(response);
        }
        return BUCKETS_ERR_RPC;
    }
    
    /* Check response */
    if (response->error_code != 0) {
        buckets_error("Remote readChunk failed: %s", 
                     response->error_message ? response->error_message : "unknown error");
        buckets_rpc_response_free(response);
        return BUCKETS_ERR_RPC;
    }
    
    /* Extract chunk data from response */
    if (!response->result) {
        buckets_error("Invalid RPC response: missing result");
        buckets_rpc_response_free(response);
        return BUCKETS_ERR_RPC;
    }
    
    cJSON *chunk_data_json = cJSON_GetObjectItem(response->result, "chunk_data");
    cJSON *chunk_size_json = cJSON_GetObjectItem(response->result, "chunk_size");
    
    if (!chunk_data_json || !chunk_size_json) {
        buckets_error("Invalid RPC response: missing chunk_data or chunk_size");
        buckets_rpc_response_free(response);
        return BUCKETS_ERR_RPC;
    }
    
    /* Decode base64 chunk data */
    size_t decoded_len = 0;
    u8 *decoded = base64_decode(chunk_data_json->valuestring, &decoded_len);
    
    *chunk_data = decoded;
    *chunk_size = decoded_len;
    
    buckets_rpc_response_free(response);
    
    buckets_debug("Distributed read: chunk %u from %s:%s (size=%zu)",
                  chunk_index, peer_endpoint, disk_path, decoded_len);
    
    return BUCKETS_OK;
}

/**
 * Write xl.meta to remote node via RPC
 * 
 * @param peer_endpoint Remote node endpoint
 * @param bucket Bucket name
 * @param object Object key
 * @param disk_path Disk path on remote node
 * @param meta xl.meta structure to write
 * @return BUCKETS_OK on success
 */
int buckets_distributed_write_xlmeta(const char *peer_endpoint,
                                      const char *bucket,
                                      const char *object,
                                      const char *disk_path,
                                      const buckets_xl_meta_t *meta)
{
    if (!g_rpc_ctx) {
        buckets_error("Distributed storage not initialized");
        return BUCKETS_ERR_INIT;
    }
    
    if (!peer_endpoint || !bucket || !object || !disk_path || !meta) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Convert xl.meta to JSON string */
    extern char* buckets_xl_meta_to_json(const buckets_xl_meta_t *meta);
    char *meta_json = buckets_xl_meta_to_json(meta);
    
    if (!meta_json) {
        buckets_error("Failed to serialize xl.meta to JSON");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Build RPC parameters */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "bucket", bucket);
    cJSON_AddStringToObject(params, "object", object);
    cJSON_AddStringToObject(params, "disk_path", disk_path);
    cJSON_AddStringToObject(params, "xl_meta_json", meta_json);
    
    buckets_free(meta_json);
    
    /* Call RPC */
    buckets_rpc_response_t *response = NULL;
    int ret = buckets_rpc_call(g_rpc_ctx, peer_endpoint, "storage.writeXlMeta",
                          params, &response, 30000);  /* 30 second timeout */
    
    cJSON_Delete(params);
    
    if (ret != BUCKETS_OK || !response) {
        buckets_error("RPC call to %s failed: storage.writeXlMeta", peer_endpoint);
        if (response) {
            buckets_rpc_response_free(response);
        }
        return BUCKETS_ERR_RPC;
    }
    
    /* Check response */
    if (response->error_code != 0) {
        buckets_error("Remote writeXlMeta failed: %s", 
                     response->error_message ? response->error_message : "unknown error");
        buckets_rpc_response_free(response);
        return BUCKETS_ERR_RPC;
    }
    
    buckets_rpc_response_free(response);
    
    buckets_debug("Distributed write xl.meta: %s/%s to %s:%s",
                  bucket, object, peer_endpoint, disk_path);
    
    return BUCKETS_OK;
}

/**
 * Read xl.meta from remote node via RPC
 * 
 * @param peer_endpoint Remote node endpoint
 * @param bucket Bucket name
 * @param object Object key
 * @param disk_path Disk path on remote node
 * @param meta Output: xl.meta structure (caller must free with buckets_xl_meta_free)
 * @return BUCKETS_OK on success
 */
int buckets_distributed_read_xlmeta(const char *peer_endpoint,
                                     const char *bucket,
                                     const char *object,
                                     const char *disk_path,
                                     buckets_xl_meta_t *meta)
{
    if (!g_rpc_ctx) {
        buckets_error("Distributed storage not initialized");
        return BUCKETS_ERR_INIT;
    }
    
    if (!peer_endpoint || !bucket || !object || !disk_path || !meta) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Build RPC parameters */
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "bucket", bucket);
    cJSON_AddStringToObject(params, "object", object);
    cJSON_AddStringToObject(params, "disk_path", disk_path);
    
    /* Call RPC */
    buckets_rpc_response_t *response = NULL;
    int ret = buckets_rpc_call(g_rpc_ctx, peer_endpoint, "storage.readXlMeta",
                              params, &response, 30000);  /* 30 second timeout */
    
    cJSON_Delete(params);
    
    if (ret != BUCKETS_OK || !response) {
        buckets_error("RPC call to %s failed: storage.readXlMeta", peer_endpoint);
        if (response) {
            buckets_rpc_response_free(response);
        }
        return BUCKETS_ERR_RPC;
    }
    
    /* Check response */
    if (response->error_code != 0) {
        buckets_error("Remote readXlMeta failed: %s", 
                     response->error_message ? response->error_message : "unknown error");
        buckets_rpc_response_free(response);
        return BUCKETS_ERR_RPC;
    }
    
    /* Extract xl.meta JSON from response */
    if (!response->result) {
        buckets_error("Invalid RPC response: missing result");
        buckets_rpc_response_free(response);
        return BUCKETS_ERR_RPC;
    }
    
    cJSON *xl_meta_json = cJSON_GetObjectItem(response->result, "xl_meta_json");
    
    if (!xl_meta_json || !cJSON_IsString(xl_meta_json)) {
        buckets_error("Invalid RPC response: missing xl_meta_json");
        buckets_rpc_response_free(response);
        return BUCKETS_ERR_RPC;
    }
    
    /* Parse xl.meta from JSON */
    extern int buckets_xl_meta_from_json(const char *json_str, buckets_xl_meta_t *meta);
    ret = buckets_xl_meta_from_json(xl_meta_json->valuestring, meta);
    
    buckets_rpc_response_free(response);
    
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to parse xl.meta JSON from remote response");
        return BUCKETS_ERR_RPC;
    }
    
    buckets_debug("Distributed read xl.meta: %s/%s from %s:%s",
                  bucket, object, peer_endpoint, disk_path);
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Distributed Delete Operations
 * ===================================================================*/

/**
 * Delete object from all distributed disks
 * 
 * Looks up placement to find all disks where the object is stored,
 * then deletes chunks and metadata from each disk (local and remote).
 */
int buckets_distributed_delete_object(const char *bucket, const char *object)
{
    if (!bucket || !object) {
        buckets_error("NULL parameter in distributed_delete_object");
        return -1;
    }
    
    /* Compute placement to find all disks */
    buckets_placement_result_t *placement = NULL;
    int ret = buckets_placement_compute(bucket, object, &placement);
    
    if (ret != 0 || !placement) {
        buckets_warn("Could not compute placement for delete: %s/%s", bucket, object);
        /* Try local-only delete as fallback */
        return buckets_delete_object(bucket, object);
    }
    
    /* Compute object path (hash-based) */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Use parallel delete for performance (deletes all shards concurrently) */
    extern int buckets_parallel_delete_chunks(const char *bucket,
                                               const char *object,
                                               const char *object_path,
                                               buckets_placement_result_t *placement);
    
    ret = buckets_parallel_delete_chunks(bucket, object, object_path, placement);
    
    /* Delete from registry (only once, not per-disk)
     * Skip registry delete if this IS a registry entry (to avoid recursion) */
    if (strcmp(bucket, ".buckets-registry") != 0) {
        const buckets_registry_config_t *reg_config = buckets_registry_get_config();
        if (reg_config) {
            if (buckets_registry_delete(bucket, object, "latest") != 0) {
                buckets_warn("Failed to delete from registry: %s/%s", bucket, object);
            }
        }
    }
    
    buckets_placement_free_result(placement);
    
    return ret;
}

/* Note: Sequential RPC delete removed - now using buckets_parallel_delete_chunks() 
 * from parallel_chunks.c for concurrent shard deletion across all disks. */
