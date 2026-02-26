/**
 * Metadata Utilities
 * 
 * ETag computation, user metadata handling, and versioning support
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uuid/uuid.h>
#include <limits.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_crypto.h"
#include "buckets_erasure.h"

/* Base64 encode for inline objects */
static char* base64_encode(const u8 *data, size_t size)
{
    static const char base64_chars[] = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    size_t encoded_len = ((size + 2) / 3) * 4;
    char *encoded = buckets_malloc(encoded_len + 1);
    
    size_t i = 0, j = 0;
    while (i < size) {
        u32 octet_a = i < size ? data[i++] : 0;
        u32 octet_b = i < size ? data[i++] : 0;
        u32 octet_c = i < size ? data[i++] : 0;
        
        u32 triple = (octet_a << 16) + (octet_b << 8) + octet_c;
        
        encoded[j++] = base64_chars[(triple >> 18) & 0x3F];
        encoded[j++] = base64_chars[(triple >> 12) & 0x3F];
        encoded[j++] = base64_chars[(triple >> 6) & 0x3F];
        encoded[j++] = base64_chars[triple & 0x3F];
    }
    
    /* Handle padding */
    size_t padding = (3 - (size % 3)) % 3;
    for (size_t k = 0; k < padding; k++) {
        encoded[encoded_len - 1 - k] = '=';
    }
    
    encoded[encoded_len] = '\0';
    return encoded;
}

/* Compute ETag for object data (BLAKE2b-256, hex-encoded) */
int buckets_compute_etag(const void *data, size_t size, char *etag)
{
    if (!data || !etag) {
        buckets_error("NULL parameter in compute_etag");
        return -1;
    }
    
    /* Compute BLAKE2b-256 hash */
    u8 hash[32];
    if (buckets_blake2b(hash, 32, data, size, NULL, 0) != 0) {
        buckets_error("Failed to compute BLAKE2b hash for ETag");
        return -1;
    }
    
    /* Convert to hex string (S3-compatible format: hex without quotes) */
    for (int i = 0; i < 32; i++) {
        sprintf(etag + (i * 2), "%02x", hash[i]);
    }
    etag[64] = '\0';
    
    buckets_debug("Computed ETag: %s", etag);
    return 0;
}

/* Add user metadata */
int buckets_add_user_metadata(buckets_xl_meta_t *meta, const char *key, const char *value)
{
    if (!meta || !key || !value) {
        buckets_error("NULL parameter in add_user_metadata");
        return -1;
    }
    
    /* Check if key already exists */
    for (u32 i = 0; i < meta->meta.user_count; i++) {
        if (strcmp(meta->meta.user_keys[i], key) == 0) {
            /* Update existing value */
            buckets_free(meta->meta.user_values[i]);
            meta->meta.user_values[i] = buckets_strdup(value);
            buckets_debug("Updated user metadata: %s=%s", key, value);
            return 0;
        }
    }
    
    /* Add new key-value pair */
    u32 new_count = meta->meta.user_count + 1;
    
    meta->meta.user_keys = buckets_realloc(meta->meta.user_keys, 
                                           new_count * sizeof(char*));
    meta->meta.user_values = buckets_realloc(meta->meta.user_values,
                                             new_count * sizeof(char*));
    
    meta->meta.user_keys[meta->meta.user_count] = buckets_strdup(key);
    meta->meta.user_values[meta->meta.user_count] = buckets_strdup(value);
    meta->meta.user_count = new_count;
    
    buckets_debug("Added user metadata: %s=%s", key, value);
    return 0;
}

/* Get user metadata value */
const char* buckets_get_user_metadata(const buckets_xl_meta_t *meta, const char *key)
{
    if (!meta || !key) {
        return NULL;
    }
    
    for (u32 i = 0; i < meta->meta.user_count; i++) {
        if (strcmp(meta->meta.user_keys[i], key) == 0) {
            return meta->meta.user_values[i];
        }
    }
    
    return NULL;  /* Not found */
}

/* Generate version ID (UUID v4) */
int buckets_generate_version_id(char *versionId)
{
    if (!versionId) {
        buckets_error("NULL versionId buffer");
        return -1;
    }
    
    uuid_t uuid;
    uuid_generate_random(uuid);
    uuid_unparse(uuid, versionId);
    
    buckets_debug("Generated version ID: %s", versionId);
    return 0;
}

/* Put object with metadata and versioning */
int buckets_put_object_with_metadata(const char *bucket, const char *object,
                                     const void *data, size_t size,
                                     buckets_xl_meta_t *provided_meta,
                                     bool enable_versioning,
                                     char *versionId)
{
    if (!bucket || !object || !data) {
        buckets_error("NULL parameter in put_object_with_metadata");
        return -1;
    }
    
    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Get storage config */
    const buckets_storage_config_t *config = buckets_storage_get_config();
    const char *disk_path = config->data_dir;
    
    /* Create object directory */
    extern int buckets_create_object_dir(const char *disk_path, const char *object_path);
    if (buckets_create_object_dir(disk_path, object_path) != 0) {
        buckets_error("Failed to create object directory");
        return -1;
    }
    
    /* Initialize metadata */
    buckets_xl_meta_t meta = {0};
    meta.version = 1;
    strcpy(meta.format, "xl");
    meta.stat.size = size;
    buckets_get_iso8601_time(meta.stat.modTime);
    
    /* Copy provided metadata if available */
    if (provided_meta) {
        /* Copy standard metadata */
        if (provided_meta->meta.content_type) {
            meta.meta.content_type = buckets_strdup(provided_meta->meta.content_type);
        }
        if (provided_meta->meta.cache_control) {
            meta.meta.cache_control = buckets_strdup(provided_meta->meta.cache_control);
        }
        if (provided_meta->meta.content_disposition) {
            meta.meta.content_disposition = buckets_strdup(provided_meta->meta.content_disposition);
        }
        if (provided_meta->meta.content_encoding) {
            meta.meta.content_encoding = buckets_strdup(provided_meta->meta.content_encoding);
        }
        if (provided_meta->meta.content_language) {
            meta.meta.content_language = buckets_strdup(provided_meta->meta.content_language);
        }
        if (provided_meta->meta.expires) {
            meta.meta.expires = buckets_strdup(provided_meta->meta.expires);
        }
        
        /* Copy user metadata */
        for (u32 i = 0; i < provided_meta->meta.user_count; i++) {
            buckets_add_user_metadata(&meta, 
                                     provided_meta->meta.user_keys[i],
                                     provided_meta->meta.user_values[i]);
        }
    }
    
    /* Compute ETag */
    char etag[65];
    if (buckets_compute_etag(data, size, etag) == 0) {
        meta.meta.etag = buckets_strdup(etag);
    }
    
    /* Handle versioning */
    if (enable_versioning) {
        char version_id[37];
        if (buckets_generate_version_id(version_id) == 0) {
            meta.versioning.versionId = buckets_strdup(version_id);
            meta.versioning.isLatest = true;
            meta.versioning.isDeleteMarker = false;
            
            /* Return version ID to caller if requested */
            if (versionId) {
                strcpy(versionId, version_id);
            }
            
            buckets_info("Object versioning enabled: %s", version_id);
        }
    }
    
    /* Store object using existing function */
    int result;
    
    /* Check if should inline */
    if (buckets_should_inline_object(size)) {
        buckets_debug("Inlining object with metadata (size=%zu)", size);
        
        /* Encode as base64 */
        meta.inline_data = base64_encode((const u8*)data, size);
        
        /* Write xl.meta only */
        result = buckets_write_xl_meta(disk_path, object_path, &meta);
    } else {
        /* Erasure encode large object */
        u32 k = config->default_ec_k;
        u32 m = config->default_ec_m;
        
        buckets_debug("Erasure encoding object with metadata: size=%zu, k=%u, m=%u", 
                     size, k, m);
        
        /* Calculate chunk size */
        size_t chunk_size = buckets_calculate_chunk_size(size, k);
        
        /* Allocate chunk arrays */
        u8 *data_chunks[k];
        u8 *parity_chunks[m];
        for (u32 i = 0; i < k; i++) {
            data_chunks[i] = buckets_malloc(chunk_size);
        }
        for (u32 i = 0; i < m; i++) {
            parity_chunks[i] = buckets_malloc(chunk_size);
        }
        
        /* Encode with erasure coding */
        buckets_ec_ctx_t ec_ctx;
        if (buckets_ec_init(&ec_ctx, k, m) != 0) {
            buckets_error("Failed to initialize erasure context");
            result = -1;
            goto cleanup_chunks;
        }
        
        if (buckets_ec_encode(&ec_ctx, data, size, chunk_size,
                             data_chunks, parity_chunks) != 0) {
            buckets_error("Failed to encode object");
            buckets_ec_free(&ec_ctx);
            result = -1;
            goto cleanup_chunks;
        }
        
        /* Set up erasure metadata */
        meta.erasure.data = k;
        meta.erasure.parity = m;
        meta.erasure.blockSize = chunk_size;
        meta.erasure.index = 1;
        strcpy(meta.erasure.algorithm, "ReedSolomon");
        
        /* Simple distribution */
        meta.erasure.distribution = buckets_malloc((k + m) * sizeof(u32));
        for (u32 i = 0; i < k + m; i++) {
            meta.erasure.distribution[i] = i + 1;
        }
        
        /* Compute checksums */
        meta.erasure.checksums = buckets_malloc((k + m) * sizeof(buckets_checksum_t));
        extern int buckets_compute_chunk_checksum(const void *data, size_t size,
                                                  buckets_checksum_t *checksum);
        for (u32 i = 0; i < k; i++) {
            buckets_compute_chunk_checksum(data_chunks[i], chunk_size,
                                          &meta.erasure.checksums[i]);
        }
        for (u32 i = 0; i < m; i++) {
            buckets_compute_chunk_checksum(parity_chunks[i], chunk_size,
                                          &meta.erasure.checksums[k + i]);
        }
        
        /* Write chunks */
        extern int buckets_write_chunk(const char *disk_path, const char *object_path,
                                      u32 chunk_index, const void *data, size_t size);
        for (u32 i = 0; i < k; i++) {
            if (buckets_write_chunk(disk_path, object_path, i + 1,
                                   data_chunks[i], chunk_size) != 0) {
                buckets_error("Failed to write data chunk %u", i);
                buckets_ec_free(&ec_ctx);
                result = -1;
                goto cleanup_chunks;
            }
        }
        for (u32 i = 0; i < m; i++) {
            if (buckets_write_chunk(disk_path, object_path, k + i + 1,
                                   parity_chunks[i], chunk_size) != 0) {
                buckets_error("Failed to write parity chunk %u", i);
                buckets_ec_free(&ec_ctx);
                result = -1;
                goto cleanup_chunks;
            }
        }
        
        /* Write xl.meta */
        result = buckets_write_xl_meta(disk_path, object_path, &meta);
        
        buckets_ec_free(&ec_ctx);
        
cleanup_chunks:
        for (u32 i = 0; i < k; i++) {
            buckets_free(data_chunks[i]);
        }
        for (u32 i = 0; i < m; i++) {
            buckets_free(parity_chunks[i]);
        }
    }
    
    buckets_xl_meta_free(&meta);
    
    if (result == 0) {
        buckets_info("Object written with metadata: %s/%s (size=%zu, versioned=%d)",
                    bucket, object, size, enable_versioning);
    }
    
    return result;
}

/* Get object by version ID */
int buckets_get_object_version(const char *bucket, const char *object,
                                const char *versionId,
                                void **data, size_t *size)
{
    if (!bucket || !object || !data || !size) {
        buckets_error("NULL parameter in get_object_version");
        return -1;
    }
    
    /* For now, if versionId is NULL, get latest version */
    /* Version-specific retrieval will be implemented in version storage */
    if (!versionId) {
        return buckets_get_object(bucket, object, data, size);
    }
    
    /* TODO: Implement version-specific retrieval */
    /* This requires storing multiple xl.meta files per version */
    buckets_warn("Version-specific retrieval not yet implemented, returning latest");
    return buckets_get_object(bucket, object, data, size);
}

/* List object versions */
int buckets_list_object_versions(const char *bucket, const char *object,
                                  char ***versions, u32 *count)
{
    if (!bucket || !object || !versions || !count) {
        buckets_error("NULL parameter in list_object_versions");
        return -1;
    }
    
    /* TODO: Implement version listing */
    /* This requires directory scanning for version-specific xl.meta files */
    buckets_warn("Version listing not yet implemented");
    *versions = NULL;
    *count = 0;
    return 0;
}
