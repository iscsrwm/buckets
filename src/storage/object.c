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
#include "buckets_registry.h"

/* Global storage configuration */
static buckets_storage_config_t g_storage_config = {
    .data_dir = NULL,
    .inline_threshold = BUCKETS_INLINE_THRESHOLD,
    .default_ec_k = 8,
    .default_ec_m = 4,
    .verify_checksums = true
};

/* Helper: Record object location in registry */
static void record_object_location(const char *bucket, const char *object, size_t size)
{
    /* Don't record registry's own objects to prevent infinite recursion */
    if (strcmp(bucket, ".buckets-registry") == 0) {
        return;
    }
    
    /* Check if registry is available (it's optional) */
    /* We check by trying to get config - if NULL, registry not initialized */
    const buckets_registry_config_t *config = buckets_registry_get_config();
    if (!config) {
        /* Registry not initialized, skip silently */
        return;
    }
    
    /* Create location record for registry */
    buckets_object_location_t loc = {0};
    loc.bucket = (char*)bucket;  /* Cast away const for struct assignment */
    loc.object = (char*)object;
    loc.version_id = "latest";   /* TODO: Use actual version_id when versioning is integrated */
    loc.pool_idx = 0;            /* TODO: Get from topology */
    loc.set_idx = 0;             /* TODO: Get from topology */
    loc.disk_count = 1;          /* TODO: Get from topology */
    loc.disk_idxs[0] = 0;        /* TODO: Get from topology */
    loc.generation = 1;          /* TODO: Get from topology */
    loc.mod_time = time(NULL);
    loc.size = size;
    
    /* Record in registry (non-fatal if fails) */
    if (buckets_registry_record(&loc) != 0) {
        buckets_warn("Failed to record object location in registry: %s/%s", bucket, object);
    }
}

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
        
        /* Record in registry if write succeeded */
        if (result == 0) {
            record_object_location(bucket, object, size);
        }
        
        buckets_xl_meta_free(&meta);
        return result;
    }

    /* Erasure encode */
    u32 k = g_storage_config.default_ec_k;
    u32 m = g_storage_config.default_ec_m;
    
    buckets_info("Erasure encoding object: size=%zu, k=%u, m=%u", size, k, m);
    
    /* Log input data for debugging */
    const u8 *input = (const u8 *)data;
    if (size >= 4) {
        buckets_info("Input data first 4 bytes: %02x %02x %02x %02x", 
                    input[0], input[1], input[2], input[3]);
    }
    if (size > 131072) {
        buckets_info("Input data at offset 131072 (4 bytes): %02x %02x %02x %02x",
                    input[131072], input[131073], input[131074], input[131075]);
    }

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

    /* Get disk paths for set 0 (we'll use topology later for set selection) */
    char *set_disk_paths[BUCKETS_EC_MAX_TOTAL];
    memset(set_disk_paths, 0, sizeof(set_disk_paths));
    
    extern int buckets_multidisk_get_set_disks(int set_index, char **disk_paths, int max_disks);
    int disk_count = buckets_multidisk_get_set_disks(0, set_disk_paths, k + m);
    
    if (disk_count < (int)(k + m)) {
        buckets_warn("Not enough disks (%d) for erasure coding (%u+%u), using single disk",
                    disk_count, k, m);
        
        /* Fallback: write all chunks to single disk */
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
        
        /* Write xl.meta to single disk */
        result = buckets_write_xl_meta(disk_path, object_path, &meta);
    } else {
        /* Distributed write: each chunk to a different disk */
        buckets_info("Writing %u data + %u parity chunks across %d disks", k, m, disk_count);
        
        /* Write data chunks */
        for (u32 i = 0; i < k; i++) {
            const char *target_disk = set_disk_paths[i];
            if (buckets_write_chunk(target_disk, object_path, i + 1, 
                                   data_chunks[i], chunk_size) != 0) {
                buckets_error("Failed to write data chunk %u to disk %s", i, target_disk);
                buckets_ec_free(&ec_ctx);
                goto cleanup_chunks;
            }
            buckets_debug("Wrote data chunk %u to disk: %s", i, target_disk);
        }
        
        /* Write parity chunks */
        for (u32 i = 0; i < m; i++) {
            const char *target_disk = set_disk_paths[k + i];
            if (buckets_write_chunk(target_disk, object_path, k + i + 1,
                                   parity_chunks[i], chunk_size) != 0) {
                buckets_error("Failed to write parity chunk %u to disk %s", i, target_disk);
                buckets_ec_free(&ec_ctx);
                goto cleanup_chunks;
            }
            buckets_debug("Wrote parity chunk %u to disk: %s", i, target_disk);
        }
        
        /* Write xl.meta to all disks */
        result = 0;
        for (u32 i = 0; i < k + m; i++) {
            meta.erasure.index = i + 1;  /* Update disk index for each disk */
            if (buckets_write_xl_meta(set_disk_paths[i], object_path, &meta) != 0) {
                buckets_error("Failed to write xl.meta to disk %s", set_disk_paths[i]);
                result = -1;
                break;
            }
        }
    }

    /* Record in registry if write succeeded */
    if (result == 0) {
        record_object_location(bucket, object, size);
    }

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

/* Get object (read) - multi-disk with erasure decoding */
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

    /* Get disk paths for erasure set 0 (SIPMOD placement) */
    char *set_disk_paths[64]; /* Max 64 disks per set */
    int set_disk_count = buckets_multidisk_get_set_disks(0, set_disk_paths, 64);
    
    if (set_disk_count > 0) {
        buckets_debug("Reading from %d disks in set 0", set_disk_count);
    } else {
        /* Fallback to single disk */
        set_disk_paths[0] = (char*)g_storage_config.data_dir;
        set_disk_count = 1;
        buckets_debug("Fallback: reading from single disk");
    }

    /* Try to read xl.meta from first available disk */
    buckets_xl_meta_t meta;
    int meta_found = 0;
    
    for (int i = 0; i < set_disk_count && !meta_found; i++) {
        if (buckets_read_xl_meta(set_disk_paths[i], object_path, &meta) == 0) {
            buckets_debug("Read xl.meta from disk %d: %s", i + 1, set_disk_paths[i]);
            meta_found = 1;
            break;
        }
    }
    
    if (!meta_found) {
        buckets_error("Failed to read xl.meta for %s/%s from any disk", bucket, object);
        return -1;
    }

    /* Check if inline */
    if (meta.inline_data) {
        buckets_debug("Reading inline object");
        *data = base64_decode(meta.inline_data, size);
        buckets_xl_meta_free(&meta);
        return 0;
    }

    /* Read chunks from distributed disks */
    u32 k = meta.erasure.data;
    u32 m = meta.erasure.parity;
    size_t chunk_size = meta.erasure.blockSize;
    u32 total_chunks = k + m;

    buckets_debug("Reading erasure-coded object: k=%u, m=%u, chunk_size=%zu, total_chunks=%u",
                  k, m, chunk_size, total_chunks);

    /* Allocate chunk pointers */
    u8 *chunks[total_chunks];
    u32 available_chunks = 0;
    
    /* Read each chunk from its corresponding disk */
    for (u32 i = 0; i < total_chunks; i++) {
        chunks[i] = NULL;
        
        /* Get disk index from erasure distribution (or use i if not present) */
        int disk_idx = (int)i;
        if (meta.erasure.distribution && i < 16 && meta.erasure.distribution[i] > 0) {
            disk_idx = (int)(meta.erasure.distribution[i] - 1); /* distribution is 1-indexed */
        }
        
        /* Ensure disk_idx is within bounds */
        if (disk_idx >= set_disk_count || disk_idx < 0) {
            buckets_warn("Chunk %u: disk_idx %d out of bounds (max %d)", 
                        i + 1, disk_idx, set_disk_count);
            continue;
        }
        
        const char *chunk_disk_path = set_disk_paths[disk_idx];
        size_t read_size;
        
        if (buckets_read_chunk(chunk_disk_path, object_path, i + 1,
                              (void**)&chunks[i], &read_size) != 0) {
            buckets_warn("Chunk %u missing from disk %u (%s)", 
                        i + 1, disk_idx + 1, chunk_disk_path);
            continue;
        }
        
        buckets_debug("Read chunk %u from disk %u (%s): %zu bytes",
                     i + 1, disk_idx + 1, chunk_disk_path, read_size);
        
        /* Verify checksum if enabled */
        if (g_storage_config.verify_checksums && meta.erasure.checksums) {
            if (!buckets_verify_chunk(chunks[i], chunk_size,
                                     &meta.erasure.checksums[i])) {
                buckets_warn("Chunk %u checksum mismatch", i + 1);
                buckets_free(chunks[i]);
                chunks[i] = NULL;
                continue;
            }
        }
        
        available_chunks++;
    }
    
    /* Check if we have enough chunks to reconstruct (need at least K) */
    if (available_chunks < k) {
        buckets_error("Not enough chunks available: %u/%u (need at least %u)", 
                     available_chunks, total_chunks, k);
        goto cleanup_read;
    }
    
    buckets_info("Successfully read %u/%u chunks (need %u for reconstruction)", 
                available_chunks, total_chunks, k);

    /* Allocate output buffer */
    *data = buckets_malloc(meta.stat.size);
    *size = meta.stat.size;

    /* Decode object */
    buckets_debug("Preparing to decode: k=%u, m=%u, chunk_size=%zu, data_size=%zu",
                 k, m, chunk_size, meta.stat.size);
    
    /* Debug: show which chunks are available */
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i]) {
            buckets_debug("  Chunk %u: available (%p)", i, (void*)chunks[i]);
        } else {
            buckets_debug("  Chunk %u: NULL", i);
        }
    }
    
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
    
    buckets_debug("Decode completed successfully");
    
    buckets_ec_free(&ec_ctx);
    buckets_info("Object read: %s/%s (size=%zu)", bucket, object, *size);

    /* Success - free chunks and metadata */
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i]) {
            buckets_free(chunks[i]);
        }
    }
    buckets_xl_meta_free(&meta);
    return 0;

cleanup_read:
    /* Error cleanup - free any allocated chunks */
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i]) {
            buckets_free(chunks[i]);
        }
    }
    buckets_xl_meta_free(&meta);

    return -1;
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
    
    /* Remove from registry (if initialized) */
    const buckets_registry_config_t *reg_config = buckets_registry_get_config();
    if (reg_config) {
        if (buckets_registry_delete(bucket, object, "latest") != 0) {
            buckets_warn("Failed to delete object location from registry: %s/%s", bucket, object);
        }
    }
    
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
