/**
 * Object Storage Implementation
 * 
 * High-level object operations integrating erasure coding, checksums,
 * and metadata management.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_erasure.h"
#include "buckets_crypto.h"
#include "buckets_hash.h"

/* Global storage configuration */
static buckets_storage_config_t g_storage_config = {
    .data_dir = NULL,
    .inline_threshold = BUCKETS_INLINE_THRESHOLD,
    .default_ec_k = 8,
    .default_ec_m = 4,
    .verify_checksums = true
};

/* Initialize storage system */
int buckets_storage_init(const buckets_storage_config_t *config)
{
    if (!config) {
        buckets_error("NULL config in storage_init");
        return -1;
    }

    /* Copy configuration */
    if (config->data_dir) {
        g_storage_config.data_dir = buckets_strdup(config->data_dir);
    } else {
        g_storage_config.data_dir = buckets_strdup(".buckets/data");
    }

    g_storage_config.inline_threshold = config->inline_threshold;
    g_storage_config.default_ec_k = config->default_ec_k;
    g_storage_config.default_ec_m = config->default_ec_m;
    g_storage_config.verify_checksums = config->verify_checksums;

    buckets_info("Storage initialized: data_dir=%s, inline_threshold=%u, ec=%u+%u",
                 g_storage_config.data_dir, 
                 g_storage_config.inline_threshold,
                 g_storage_config.default_ec_k,
                 g_storage_config.default_ec_m);

    return 0;
}

/* Cleanup storage system */
void buckets_storage_cleanup(void)
{
    if (g_storage_config.data_dir) {
        buckets_free(g_storage_config.data_dir);
        g_storage_config.data_dir = NULL;
    }
}

/* Get current storage configuration */
const buckets_storage_config_t* buckets_storage_get_config(void)
{
    return &g_storage_config;
}

/* Base64 encode for inline data */
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

/* Base64 decode from inline data */
static u8* base64_decode(const char *encoded, size_t *decoded_size)
{
    static const u8 base64_table[256] = {
        ['A'] = 0,  ['B'] = 1,  ['C'] = 2,  ['D'] = 3,  ['E'] = 4,
        ['F'] = 5,  ['G'] = 6,  ['H'] = 7,  ['I'] = 8,  ['J'] = 9,
        ['K'] = 10, ['L'] = 11, ['M'] = 12, ['N'] = 13, ['O'] = 14,
        ['P'] = 15, ['Q'] = 16, ['R'] = 17, ['S'] = 18, ['T'] = 19,
        ['U'] = 20, ['V'] = 21, ['W'] = 22, ['X'] = 23, ['Y'] = 24,
        ['Z'] = 25, ['a'] = 26, ['b'] = 27, ['c'] = 28, ['d'] = 29,
        ['e'] = 30, ['f'] = 31, ['g'] = 32, ['h'] = 33, ['i'] = 34,
        ['j'] = 35, ['k'] = 36, ['l'] = 37, ['m'] = 38, ['n'] = 39,
        ['o'] = 40, ['p'] = 41, ['q'] = 42, ['r'] = 43, ['s'] = 44,
        ['t'] = 45, ['u'] = 46, ['v'] = 47, ['w'] = 48, ['x'] = 49,
        ['y'] = 50, ['z'] = 51, ['0'] = 52, ['1'] = 53, ['2'] = 54,
        ['3'] = 55, ['4'] = 56, ['5'] = 57, ['6'] = 58, ['7'] = 59,
        ['8'] = 60, ['9'] = 61, ['+'] = 62, ['/'] = 63
    };
    
    size_t len = strlen(encoded);
    size_t padding = 0;
    if (len > 0 && encoded[len-1] == '=') padding++;
    if (len > 1 && encoded[len-2] == '=') padding++;
    
    *decoded_size = (len / 4) * 3 - padding;
    u8 *decoded = buckets_malloc(*decoded_size);
    
    size_t i = 0, j = 0;
    while (i < len) {
        u32 sextet_a = encoded[i] == '=' ? 0 : base64_table[(u8)encoded[i]]; i++;
        u32 sextet_b = encoded[i] == '=' ? 0 : base64_table[(u8)encoded[i]]; i++;
        u32 sextet_c = encoded[i] == '=' ? 0 : base64_table[(u8)encoded[i]]; i++;
        u32 sextet_d = encoded[i] == '=' ? 0 : base64_table[(u8)encoded[i]]; i++;
        
        u32 triple = (sextet_a << 18) + (sextet_b << 12) + (sextet_c << 6) + sextet_d;
        
        if (j < *decoded_size) decoded[j++] = (triple >> 16) & 0xFF;
        if (j < *decoded_size) decoded[j++] = (triple >> 8) & 0xFF;
        if (j < *decoded_size) decoded[j++] = triple & 0xFF;
    }
    
    return decoded;
}

/* Put object (write) - simplified single-disk version */
int buckets_put_object(const char *bucket, const char *object,
                       const void *data, size_t size,
                       const char *content_type)
{
    int result = -1;  /* Initialize to error by default */
    
    if (!bucket || !object || !data) {
        buckets_error("NULL parameter in put_object");
        return -1;
    }

    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));

    /* For now, use single disk (will integrate topology later) */
    const char *disk_path = g_storage_config.data_dir;

    /* Create object directory */
    char full_path[PATH_MAX * 2];
    snprintf(full_path, sizeof(full_path), "%s/%s", disk_path, object_path);
    
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

    if (content_type) {
        meta.meta.content_type = buckets_strdup(content_type);
    }

    /* Check if should inline */
    if (buckets_should_inline_object(size)) {
        buckets_debug("Inlining object (size=%zu)", size);
        
        /* Encode as base64 */
        meta.inline_data = base64_encode((const u8*)data, size);
        
        /* Write xl.meta only */
        result = buckets_write_xl_meta(disk_path, object_path, &meta);
        
        buckets_xl_meta_free(&meta);
        return result;
    }

    /* Erasure encode */
    u32 k = g_storage_config.default_ec_k;
    u32 m = g_storage_config.default_ec_m;
    
    buckets_debug("Erasure encoding object: size=%zu, k=%u, m=%u", size, k, m);

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
        goto cleanup_chunks;
    }

    if (buckets_ec_encode(&ec_ctx, data, size, chunk_size,
                          data_chunks, parity_chunks) != 0) {
        buckets_error("Failed to encode object");
        buckets_ec_free(&ec_ctx);
        goto cleanup_chunks;
    }

    /* Compute checksums */
    meta.erasure.data = k;
    meta.erasure.parity = m;
    meta.erasure.blockSize = chunk_size;
    meta.erasure.index = 1;  /* Single disk for now */
    strcpy(meta.erasure.algorithm, "ReedSolomon");

    /* Simple distribution (sequential) */
    meta.erasure.distribution = buckets_malloc((k + m) * sizeof(u32));
    for (u32 i = 0; i < k + m; i++) {
        meta.erasure.distribution[i] = i + 1;
    }

    /* Compute checksums for all chunks */
    meta.erasure.checksums = buckets_malloc((k + m) * sizeof(buckets_checksum_t));
    
    for (u32 i = 0; i < k; i++) {
        extern int buckets_compute_chunk_checksum(const void *data, size_t size,
                                                   buckets_checksum_t *checksum);
        buckets_compute_chunk_checksum(data_chunks[i], chunk_size, 
                                       &meta.erasure.checksums[i]);
    }
    for (u32 i = 0; i < m; i++) {
        extern int buckets_compute_chunk_checksum(const void *data, size_t size,
                                                   buckets_checksum_t *checksum);
        buckets_compute_chunk_checksum(parity_chunks[i], chunk_size,
                                       &meta.erasure.checksums[k + i]);
    }

    /* Write chunks */
    for (u32 i = 0; i < k; i++) {
        if (buckets_write_chunk(disk_path, object_path, i + 1, 
                               data_chunks[i], chunk_size) != 0) {
            buckets_error("Failed to write data chunk %u", i);
            buckets_ec_free(&ec_ctx);
            goto cleanup_chunks;
        }
    }
    for (u32 i = 0; i < m; i++) {
        if (buckets_write_chunk(disk_path, object_path, k + i + 1,
                               parity_chunks[i], chunk_size) != 0) {
            buckets_error("Failed to write parity chunk %u", i);
            buckets_ec_free(&ec_ctx);
            goto cleanup_chunks;
        }
    }

    /* Write xl.meta */
    result = buckets_write_xl_meta(disk_path, object_path, &meta);

    /* Cleanup */
    buckets_ec_free(&ec_ctx);

cleanup_chunks:
    for (u32 i = 0; i < k; i++) {
        buckets_free(data_chunks[i]);
    }
    for (u32 i = 0; i < m; i++) {
        buckets_free(parity_chunks[i]);
    }
    buckets_xl_meta_free(&meta);

    buckets_info("Object written: %s/%s (size=%zu)", bucket, object, size);
    return result;
}

/* Get object (read) - simplified single-disk version */
int buckets_get_object(const char *bucket, const char *object,
                       void **data, size_t *size)
{
    if (!bucket || !object || !data || !size) {
        buckets_error("NULL parameter in get_object");
        return -1;
    }

    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));

    const char *disk_path = g_storage_config.data_dir;

    /* Read xl.meta */
    buckets_xl_meta_t meta;
    if (buckets_read_xl_meta(disk_path, object_path, &meta) != 0) {
        buckets_error("Failed to read xl.meta for %s/%s", bucket, object);
        return -1;
    }

    /* Check if inline */
    if (meta.inline_data) {
        buckets_debug("Reading inline object");
        *data = base64_decode(meta.inline_data, size);
        buckets_xl_meta_free(&meta);
        return 0;
    }

    /* Read chunks */
    u32 k = meta.erasure.data;
    u32 m = meta.erasure.parity;
    size_t chunk_size = meta.erasure.blockSize;

    buckets_debug("Reading erasure-coded object: k=%u, m=%u, chunk_size=%zu",
                  k, m, chunk_size);

    /* Allocate chunk pointers */
    u8 *chunks[k + m];
    for (u32 i = 0; i < k + m; i++) {
        size_t read_size;
        if (buckets_read_chunk(disk_path, object_path, i + 1,
                              (void**)&chunks[i], &read_size) != 0) {
            buckets_warn("Chunk %u missing or unreadable", i + 1);
            chunks[i] = NULL;
        } else {
            /* Verify checksum if enabled */
            if (g_storage_config.verify_checksums) {
                if (!buckets_verify_chunk(chunks[i], chunk_size,
                                         &meta.erasure.checksums[i])) {
                    buckets_warn("Chunk %u checksum mismatch", i + 1);
                    buckets_free(chunks[i]);
                    chunks[i] = NULL;
                }
            }
        }
    }

    /* Allocate output buffer */
    *data = buckets_malloc(meta.stat.size);
    *size = meta.stat.size;

    /* Decode object */
    buckets_ec_ctx_t ec_ctx;
    if (buckets_ec_init(&ec_ctx, k, m) != 0) {
        buckets_error("Failed to initialize erasure context");
        buckets_free(*data);
        *data = NULL;
        goto cleanup_read;
    }

    if (buckets_ec_decode(&ec_ctx, chunks, chunk_size, *data, meta.stat.size) != 0) {
        buckets_error("Failed to decode object");
        buckets_ec_free(&ec_ctx);
        buckets_free(*data);
        *data = NULL;
        goto cleanup_read;
    }
    
    buckets_ec_free(&ec_ctx);
    buckets_info("Object read: %s/%s (size=%zu)", bucket, object, *size);

cleanup_read:
    /* Free chunks */
    for (u32 i = 0; i < k + m; i++) {
        if (chunks[i]) {
            buckets_free(chunks[i]);
        }
    }
    buckets_xl_meta_free(&meta);

    return 0;
}

/* Delete object */
int buckets_delete_object(const char *bucket, const char *object)
{
    if (!bucket || !object) {
        buckets_error("NULL parameter in delete_object");
        return -1;
    }

    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));

    const char *disk_path = g_storage_config.data_dir;

    /* Read xl.meta to get chunk count */
    buckets_xl_meta_t meta;
    if (buckets_read_xl_meta(disk_path, object_path, &meta) != 0) {
        buckets_error("Failed to read xl.meta for delete");
        return -1;
    }

    /* Delete chunks (if not inline) */
    if (!meta.inline_data) {
        u32 total_chunks = meta.erasure.data + meta.erasure.parity;
        for (u32 i = 0; i < total_chunks; i++) {
            extern int buckets_delete_chunk(const char *disk_path, 
                                           const char *object_path, u32 chunk_index);
            buckets_delete_chunk(disk_path, object_path, i + 1);
        }
    }

    /* Delete xl.meta */
    char meta_path[PATH_MAX * 2];
    snprintf(meta_path, sizeof(meta_path), "%s/%sxl.meta", disk_path, object_path);
    unlink(meta_path);

    /* Try to remove directory (will fail if not empty) */
    char dir_path[PATH_MAX * 2];
    snprintf(dir_path, sizeof(dir_path), "%s/%s", disk_path, object_path);
    /* Remove trailing slash */
    size_t len = strlen(dir_path);
    if (len > 0 && dir_path[len-1] == '/') {
        dir_path[len-1] = '\0';
    }
    rmdir(dir_path);

    buckets_xl_meta_free(&meta);
    buckets_info("Object deleted: %s/%s", bucket, object);

    return 0;
}

/* Head object (metadata only) */
int buckets_head_object(const char *bucket, const char *object,
                        buckets_xl_meta_t *meta)
{
    if (!bucket || !object || !meta) {
        buckets_error("NULL parameter in head_object");
        return -1;
    }

    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));

    const char *disk_path = g_storage_config.data_dir;

    return buckets_read_xl_meta(disk_path, object_path, meta);
}

/* Stat object (size and modTime only) */
int buckets_stat_object(const char *bucket, const char *object,
                        size_t *size, char *modTime)
{
    if (!bucket || !object || !size || !modTime) {
        buckets_error("NULL parameter in stat_object");
        return -1;
    }

    buckets_xl_meta_t meta;
    if (buckets_head_object(bucket, object, &meta) != 0) {
        return -1;
    }

    *size = meta.stat.size;
    strncpy(modTime, meta.stat.modTime, 32);

    buckets_xl_meta_free(&meta);
    return 0;
}
