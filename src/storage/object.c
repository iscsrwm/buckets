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
#include "buckets_placement.h"

/* Global storage configuration */
static buckets_storage_config_t g_storage_config = {
    .data_dir = NULL,
    .inline_threshold = BUCKETS_INLINE_THRESHOLD,
    .default_ec_k = 0,
    .default_ec_m = 0,
    .verify_checksums = true
};

/**
 * Get storage data directory
 */
int buckets_get_data_dir(char *data_dir, size_t size)
{
    if (!data_dir || size == 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (!g_storage_config.data_dir || g_storage_config.data_dir[0] == '\0') {
        /* No data directory configured, use default */
        snprintf(data_dir, size, "/tmp/buckets-data");
    } else {
        snprintf(data_dir, size, "%s", g_storage_config.data_dir);
    }
    
    return BUCKETS_OK;
}

/* Helper: Record object location in registry */
static void record_object_location(const char *bucket, const char *object, size_t size,
                                   buckets_placement_result_t *placement)
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
    
    /* Create location record for registry using actual placement */
    buckets_object_location_t loc = {0};
    loc.bucket = (char*)bucket;  /* Cast away const for struct assignment */
    loc.object = (char*)object;
    loc.version_id = "latest";   /* TODO: Use actual version_id when versioning is integrated */
    
    if (placement) {
        /* Use actual topology information from placement */
        loc.pool_idx = placement->pool_idx;
        loc.set_idx = placement->set_idx;
        loc.disk_count = placement->disk_count < BUCKETS_REGISTRY_MAX_DISKS ? 
                         placement->disk_count : BUCKETS_REGISTRY_MAX_DISKS;
        for (u32 i = 0; i < loc.disk_count; i++) {
            loc.disk_idxs[i] = i;  /* Sequential disk indices within set */
        }
        loc.generation = placement->generation;
    } else {
        /* Fallback to defaults if no placement */
        loc.pool_idx = 0;
        loc.set_idx = 0;
        loc.disk_count = 1;
        loc.disk_idxs[0] = 0;
        loc.generation = 1;
    }
    
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

/* Put object (write) - with SIPMOD placement and distributed erasure coding */
int buckets_put_object(const char *bucket, const char *object,
                       const void *data, size_t size,
                       const char *content_type)
{
    struct timespec start_total, end_total;
    clock_gettime(CLOCK_MONOTONIC, &start_total);
    
    int result = -1;  /* Initialize to error by default */
    
    if (!bucket || !object || !data) {
        buckets_error("NULL parameter in put_object");
        return -1;
    }

    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));

    /* Compute placement using consistent hashing */
    buckets_placement_result_t *placement = NULL;
    if (buckets_placement_compute(bucket, object, &placement) == 0) {
        buckets_info("Placement computed: pool=%u, set=%u, disks=%u, hash=%016llx, vnode=%u",
                     placement->pool_idx, placement->set_idx, placement->disk_count,
                     (unsigned long long)placement->object_hash, placement->vnode_index);
        if (placement->disk_count > 0) {
            buckets_info("Placement disk_paths[0]: %s", placement->disk_paths[0]);
        }
    } else {
        buckets_warn("Failed to compute placement, using fallback");
    }

    /* Determine disk path (use placement if available, otherwise fallback) */
    const char *disk_path = NULL;
    if (placement && placement->disk_count > 0) {
        disk_path = placement->disk_paths[0];
        buckets_info("Using placement disk path: %s", disk_path);
    } else {
        disk_path = g_storage_config.data_dir;
        buckets_info("Using config data_dir: %s", disk_path ? disk_path : "(null)");
    }
    
    /* Fallback to /tmp/buckets-data if storage not initialized (for tests) */
    if (!disk_path || disk_path[0] == '\0') {
        disk_path = "/tmp/buckets-data";
        buckets_info("Fallback to default: %s", disk_path);
    }

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
            record_object_location(bucket, object, size, placement);
        }
        
        buckets_xl_meta_free(&meta);
        buckets_placement_free_result(placement);
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
    struct timespec start_encode, end_encode;
    clock_gettime(CLOCK_MONOTONIC, &start_encode);
    
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
    
    clock_gettime(CLOCK_MONOTONIC, &end_encode);
    double encode_time = (end_encode.tv_sec - start_encode.tv_sec) + 
                        (end_encode.tv_nsec - start_encode.tv_nsec) / 1e9;
    buckets_info("⏱️  Erasure encoding: %.3f ms (%.2f MB/s)", 
                 encode_time * 1000, (size / 1024.0 / 1024.0) / encode_time);

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

    /* Get disk paths from placement (SIPMOD-based set selection) */
    char **set_disk_paths = NULL;
    int disk_count = 0;
    
    if (placement && placement->disk_count >= k + m) {
        /* Use placement disk paths */
        set_disk_paths = placement->disk_paths;
        disk_count = placement->disk_count;
        buckets_debug("Using placement disks: %d disks in set %u", disk_count, placement->set_idx);
    } else {
        /* Fallback: try to get disks from multi-disk layer */
        extern int buckets_multidisk_get_set_disks(int set_index, char **disk_paths, int max_disks);
        static char *fallback_disk_paths[BUCKETS_EC_MAX_TOTAL];
        disk_count = buckets_multidisk_get_set_disks(0, fallback_disk_paths, k + m);
        set_disk_paths = fallback_disk_paths;
        buckets_debug("Using fallback disks: %d disks", disk_count);
    }
    
    /* Use single-disk fallback if:
     * 1. Not enough disks for erasure coding, OR
     * 2. Placement is NULL (can't use parallel write without placement info)
     */
    if (disk_count < (int)(k + m) || !placement) {
        if (!placement) {
            buckets_warn("No placement available, using single disk write");
        } else {
            buckets_warn("Not enough disks (%d) for erasure coding (%u+%u), using single disk",
                        disk_count, k, m);
        }
        
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
        /* Distributed write: each chunk to a different disk (local or remote via RPC) */
        buckets_info("Writing %u data + %u parity chunks across %d disks (PARALLEL)", k, m, disk_count);
        
        /* Check if we have disk endpoints for distributed RPC */
        bool has_endpoints = (placement && placement->disk_endpoints && 
                             placement->disk_endpoints[0] && 
                             placement->disk_endpoints[0][0] != '\0');
        
        /* Prepare chunk data array (data + parity) */
        const void **chunk_array = buckets_malloc((k + m) * sizeof(void*));
        if (!chunk_array) {
            buckets_error("Failed to allocate chunk array");
            buckets_ec_free(&ec_ctx);
            goto cleanup_chunks;
        }
        
        for (u32 i = 0; i < k + m; i++) {
            chunk_array[i] = (i < k) ? data_chunks[i] : parity_chunks[i - k];
        }
        
        /* Write all chunks in parallel */
        struct timespec start_write, end_write;
        clock_gettime(CLOCK_MONOTONIC, &start_write);
        
        extern int buckets_parallel_write_chunks(const char *bucket, const char *object,
                                                 const char *object_path,
                                                 buckets_placement_result_t *placement,
                                                 const void **chunk_data_array,
                                                 size_t chunk_size, u32 num_chunks);
        
        int write_result = buckets_parallel_write_chunks(bucket, object, object_path, placement,
                                                         chunk_array, chunk_size, k + m);
        
        clock_gettime(CLOCK_MONOTONIC, &end_write);
        double write_time = (end_write.tv_sec - start_write.tv_sec) + 
                           (end_write.tv_nsec - start_write.tv_nsec) / 1e9;
        
        buckets_free(chunk_array);
        
        if (write_result != 0) {
            buckets_error("Parallel chunk write failed");
            buckets_ec_free(&ec_ctx);
            goto cleanup_chunks;
        }
        
        buckets_info("⏱️  Parallel chunk writes: %.3f ms (%.2f MB/s)", 
                     write_time * 1000, (size / 1024.0 / 1024.0) / write_time);
        buckets_info("Parallel write complete: %u chunks written", k + m);
        
        /* Write xl.meta to all disks (local and remote) in PARALLEL */
        struct timespec start_meta, end_meta;
        clock_gettime(CLOCK_MONOTONIC, &start_meta);
        
        extern int buckets_parallel_write_metadata(const char *bucket,
                                                   const char *object,
                                                   const char *object_path,
                                                   buckets_placement_result_t *placement,
                                                   char **disk_paths,
                                                   const buckets_xl_meta_t *base_meta,
                                                   u32 num_disks,
                                                   bool has_endpoints);
        
        result = buckets_parallel_write_metadata(bucket, object, object_path,
                                                 placement, set_disk_paths, &meta,
                                                 k + m, has_endpoints);
        
        clock_gettime(CLOCK_MONOTONIC, &end_meta);
        double meta_time = (end_meta.tv_sec - start_meta.tv_sec) + 
                          (end_meta.tv_nsec - start_meta.tv_nsec) / 1e9;
        buckets_info("⏱️  Metadata writes (PARALLEL): %.3f ms (%u disks)", meta_time * 1000, k + m);
    }

    /* Record in registry if write succeeded */
    if (result == 0) {
        record_object_location(bucket, object, size, placement);
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
    buckets_placement_free_result(placement);

    clock_gettime(CLOCK_MONOTONIC, &end_total);
    double total_time = (end_total.tv_sec - start_total.tv_sec) + 
                       (end_total.tv_nsec - start_total.tv_nsec) / 1e9;
    buckets_info("⏱️  TOTAL upload time: %.3f ms (%.2f MB/s) for %s/%s", 
                 total_time * 1000, (size / 1024.0 / 1024.0) / total_time, bucket, object);
    buckets_info("Object written: %s/%s (size=%zu)", bucket, object, size);
    return result;
}

/* Get object (read) - with registry lookup and multi-disk erasure decoding */
int buckets_get_object(const char *bucket, const char *object,
                       void **data, size_t *size)
{
    buckets_debug("GET object: %s/%s", bucket ? bucket : "(null)", 
                  object ? object : "(null)");
    
    if (!bucket || !object || !data || !size) {
        buckets_error("NULL parameter in get_object");
        return -1;
    }

    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    buckets_debug("Object path: %s", object_path);

    /* Try registry lookup first to find object location */
    buckets_object_location_t *location = NULL;
    buckets_placement_result_t *placement = NULL;
    char **set_disk_paths = NULL;
    int set_disk_count = 0;
    bool registry_hit = false;
    
    /* Skip registry lookup for the registry bucket itself to avoid infinite recursion:
     * buckets_registry_lookup() calls buckets_get_object() for cache misses,
     * which would call buckets_registry_lookup() again, causing stack overflow.
     */
    bool skip_registry = (strcmp(bucket, BUCKETS_REGISTRY_BUCKET) == 0);
    
    if (!skip_registry && buckets_registry_lookup(bucket, object, NULL, &location) == 0) {
        buckets_debug("Registry hit: pool=%u, set=%u, disks=%u",
                     location->pool_idx, location->set_idx, location->disk_count);
        registry_hit = true;
        
        /* Get disk paths from topology using location info */
        if (buckets_placement_compute(bucket, object, &placement) == 0) {
            if (placement->set_idx == location->set_idx) {
                set_disk_paths = placement->disk_paths;
                set_disk_count = placement->disk_count;
                buckets_debug("Using placement disks for set %u: %d disks",
                             placement->set_idx, set_disk_count);
            }
            /* Don't free placement yet - we're using its disk_paths */
        }
    }
    
    /* Fallback: compute placement if registry missed or failed */
    if (!registry_hit || set_disk_count == 0) {
        buckets_info("GET_OBJECT_DEBUG: Registry miss, computing placement");
        buckets_debug("Registry miss, computing placement");
        
        if (buckets_placement_compute(bucket, object, &placement) == 0) {
            set_disk_paths = placement->disk_paths;
            set_disk_count = placement->disk_count;
            buckets_debug("Using computed placement: set=%u, %d disks",
                         placement->set_idx, set_disk_count);
            /* Don't free placement yet */
        }
    }
    
    /* Final fallback: try multi-disk layer or single disk */
    if (set_disk_count == 0) {
        extern int buckets_multidisk_get_set_disks(int set_index, char **disk_paths, int max_disks);
        static char *fallback_paths[64];
        set_disk_count = buckets_multidisk_get_set_disks(0, fallback_paths, 64);
        
        if (set_disk_count > 0) {
            set_disk_paths = fallback_paths;
            buckets_debug("Fallback: using multi-disk layer, %d disks", set_disk_count);
        } else {
            /* Last resort: single disk */
            static char *single_disk_path;
            single_disk_path = (char*)g_storage_config.data_dir;
            if (!single_disk_path || single_disk_path[0] == '\0') {
                single_disk_path = "/tmp/buckets-data";
            }
            set_disk_paths = &single_disk_path;
            set_disk_count = 1;
            buckets_debug("Fallback: using single disk");
        }
    }
    
    if (location) {
        buckets_registry_location_free(location);
    }

    /* Try to read xl.meta from first available disk (local or remote) */
    buckets_xl_meta_t meta;
    int meta_found = 0;
    
    buckets_debug("Reading xl.meta: set_disk_count=%d, placement=%p", 
                  set_disk_count, (void*)placement);
    
    /* Check if we have disk endpoints for distributed reads */
    bool has_endpoints = (placement && placement->disk_endpoints && 
                         placement->disk_endpoints[0] && 
                         placement->disk_endpoints[0][0] != '\0');
    
    for (int i = 0; i < set_disk_count && !meta_found; i++) {
        /* Try local read first */
        if (buckets_read_xl_meta(set_disk_paths[i], object_path, &meta) == 0) {
            buckets_debug("Read xl.meta from local disk %d: %s", i + 1, set_disk_paths[i]);
            meta_found = 1;
            break;
        }
        
        /* If local read failed and we have remote endpoints, try RPC read */
        if (!meta_found && has_endpoints) {
            /* Check endpoint bounds before accessing */
            if (i >= (int)placement->disk_count || !placement->disk_endpoints[i]) {
                buckets_warn("Skipping disk %d - out of bounds or NULL endpoint", i);
                continue;
            }
            
            if (!buckets_distributed_is_local_disk(placement->disk_endpoints[i])) {
                char node_endpoint[256];
                extern int buckets_distributed_extract_node_endpoint(const char *disk_endpoint, 
                                                                     char *node_endpoint, size_t size);
                
                if (buckets_distributed_extract_node_endpoint(placement->disk_endpoints[i], 
                                                              node_endpoint, sizeof(node_endpoint)) == 0) {
                    /* Call RPC method to read xl.meta from remote disk */
                    extern int buckets_distributed_read_xlmeta(const char *peer_endpoint,
                                                              const char *bucket, const char *object,
                                                              const char *disk_path,
                                                              buckets_xl_meta_t *meta);
                    
                    int ret = buckets_distributed_read_xlmeta(node_endpoint, bucket, object,
                                                             set_disk_paths[i], &meta);
                    if (ret == 0) {
                        buckets_debug("Read xl.meta from remote disk %d via RPC: %s:%s", 
                                     i + 1, node_endpoint, set_disk_paths[i]);
                        meta_found = 1;
                        break;
                    }
                }
            }
        }
    }
    
    if (!meta_found) {
        buckets_error("Failed to read xl.meta for %s/%s from any disk (local or remote)", 
                     bucket, object);
        return -1;
    }

    /* Check if inline */
    if (meta.inline_data) {
        buckets_debug("Reading inline object");
        *data = base64_decode(meta.inline_data, size);
        buckets_xl_meta_free(&meta);
        if (placement) {
            buckets_placement_free_result(placement);
        }
        return 0;
    }

    /* Read chunks from distributed disks */
    u32 k = meta.erasure.data;
    u32 m = meta.erasure.parity;
    size_t chunk_size = meta.erasure.blockSize;
    u32 total_chunks = k + m;

    buckets_debug("Reading erasure-coded object: k=%u, m=%u, chunk_size=%zu, total_chunks=%u (PARALLEL)",
                  k, m, chunk_size, total_chunks);

    /* Allocate chunk arrays */
    u8 *chunks[total_chunks];
    size_t chunk_sizes[total_chunks];
    
    /* Initialize chunk arrays */
    for (u32 i = 0; i < total_chunks; i++) {
        chunks[i] = NULL;
        chunk_sizes[i] = 0;
    }
    
    int available_chunks = 0;
    
    /* Use parallel reads if placement is available, otherwise sequential */
    if (placement) {
        /* Read all chunks in parallel */
        extern int buckets_parallel_read_chunks(const char *bucket, const char *object,
                                                const char *object_path,
                                                buckets_placement_result_t *placement,
                                                void **chunk_data_array,
                                                size_t *chunk_sizes_array,
                                                u32 num_chunks);
        
        available_chunks = buckets_parallel_read_chunks(bucket, object, object_path, placement,
                                                            (void**)chunks, chunk_sizes, total_chunks);
        
        if (available_chunks < 0) {
            buckets_error("Parallel chunk read failed");
            buckets_xl_meta_free(&meta);
            buckets_placement_free_result(placement);
            return -1;
        }
    } else {
        /* No placement - read chunks sequentially from single disk */
        buckets_warn("No placement available, using sequential single-disk read");
        
        extern int buckets_read_chunk(const char *disk_path, const char *object_path,
                                      u32 chunk_index, void **data, size_t *size);
        
        const char *disk_path = set_disk_paths[0];
        
        for (u32 i = 0; i < total_chunks; i++) {
            void *chunk_data = NULL;
            size_t chunk_len = 0;
            
            if (buckets_read_chunk(disk_path, object_path, i + 1, &chunk_data, &chunk_len) == 0) {
                chunks[i] = chunk_data;
                chunk_sizes[i] = chunk_len;
                available_chunks++;
            }
        }
    }
    
    u32 available_chunks_u32 = (u32)available_chunks;
    buckets_info("Parallel read: %u/%u chunks available", available_chunks_u32, total_chunks);
    
    /* Check if we have enough chunks to reconstruct (need at least K) */
    if (available_chunks_u32 < k) {
        buckets_error("Not enough chunks available: %u/%u (need at least %u)", 
                     available_chunks_u32, total_chunks, k);
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
    if (placement) {
        buckets_placement_free_result(placement);
    }
    return 0;

cleanup_read:
    /* Error cleanup - free any allocated chunks */
    for (u32 i = 0; i < total_chunks; i++) {
        if (chunks[i]) {
            buckets_free(chunks[i]);
        }
    }
    buckets_xl_meta_free(&meta);
    if (placement) {
        buckets_placement_free_result(placement);
    }

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
