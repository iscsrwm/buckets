/**
 * Storage Layout Implementation
 * 
 * Path computation and directory management for object storage.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_hash.h"
#include "buckets_erasure.h"

/* Compute object path from bucket + object key */
void buckets_compute_object_path(const char *bucket, const char *object,
                                  char *path, size_t path_len)
{
    if (!bucket || !object || !path) {
        buckets_error("NULL parameter in compute_object_path");
        return;
    }

    /* Combine bucket and object into full key */
    char object_key[1024];
    snprintf(object_key, sizeof(object_key), "%s/%s", bucket, object);

    /* Compute hash of object key */
    char object_hash[17];
    buckets_compute_object_hash(object_key, object_hash, sizeof(object_hash));

    /* Extract hash prefix (first 2 chars) */
    char prefix[3];
    prefix[0] = object_hash[0];
    prefix[1] = object_hash[1];
    prefix[2] = '\0';

    /* Construct relative path: <prefix>/<hash>/ */
    /* The caller will prepend the disk path */
    snprintf(path, path_len, "%s/%s/", prefix, object_hash);
}

/* Compute hash prefix (00-ff) */
void buckets_compute_hash_prefix(u64 hash, char *prefix, size_t prefix_len)
{
    if (!prefix || prefix_len < 3) {
        buckets_error("Invalid prefix buffer");
        return;
    }

    /* Extract top byte for prefix */
    snprintf(prefix, prefix_len, "%02x", (unsigned int)((hash >> 56) & 0xFF));
}

/* Compute full object hash (16 hex chars) */
void buckets_compute_object_hash(const char *object_key, char *hash, size_t hash_len)
{
    if (!object_key || !hash || hash_len < 17) {
        buckets_error("Invalid parameters in compute_object_hash");
        return;
    }

    /* Use xxHash-64 with deployment ID as seed (from topology) */
    /* For now, use a fixed seed - will integrate with topology later */
    u64 seed = 0x0123456789ABCDEF;  /* TODO: Get from deployment ID */
    u64 hash_value = buckets_xxhash64(seed, object_key, strlen(object_key));

    /* Convert to 16 hex characters */
    snprintf(hash, hash_len, "%016lx", (unsigned long)hash_value);
}

/* Create directory hierarchy for object */
int buckets_create_object_dir(const char *disk_path, const char *object_path)
{
    if (!disk_path || !object_path) {
        buckets_error("NULL parameter in create_object_dir");
        return -1;
    }

    /* Construct full path */
    char full_path[PATH_MAX];
    snprintf(full_path, sizeof(full_path), "%s/%s", disk_path, object_path);

    /* Create directory recursively */
    char tmp_path[PATH_MAX];
    char *p = NULL;
    size_t len;

    snprintf(tmp_path, sizeof(tmp_path), "%s", full_path);
    len = strlen(tmp_path);
    
    /* Remove trailing slash */
    if (tmp_path[len - 1] == '/') {
        tmp_path[len - 1] = '\0';
    }

    /* Create directories one by one */
    for (p = tmp_path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            
            if (mkdir(tmp_path, 0755) != 0 && errno != EEXIST) {
                buckets_error("Failed to create directory %s: %s",
                            tmp_path, strerror(errno));
                return -1;
            }
            
            *p = '/';
        }
    }

    /* Create final directory */
    if (mkdir(tmp_path, 0755) != 0 && errno != EEXIST) {
        buckets_error("Failed to create directory %s: %s",
                    tmp_path, strerror(errno));
        return -1;
    }

    return 0;
}

/* Check if object exists */
bool buckets_object_exists(const char *disk_path, const char *object_path)
{
    if (!disk_path || !object_path) {
        return false;
    }

    /* Check if xl.meta exists */
    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/%s/xl.meta", 
             disk_path, object_path);

    struct stat st;
    return (stat(meta_path, &st) == 0);
}

/* Get object directory full path */
void buckets_get_object_full_path(const char *disk_path, const char *object_path,
                                   char *full_path, size_t full_path_len)
{
    if (!disk_path || !object_path || !full_path) {
        buckets_error("NULL parameter in get_object_full_path");
        return;
    }

    snprintf(full_path, full_path_len, "%s/%s", disk_path, object_path);
}

/* Calculate optimal chunk size */
size_t buckets_calculate_chunk_size(size_t object_size, u32 k)
{
    if (k == 0) {
        buckets_error("Invalid k=0 in calculate_chunk_size");
        return BUCKETS_MIN_CHUNK_SIZE;
    }

    /* Use erasure coding helper (from Week 10-11) */
    size_t chunk_size = buckets_ec_calc_chunk_size(object_size, k);

    /* Clamp to reasonable range */
    if (chunk_size < BUCKETS_MIN_CHUNK_SIZE) {
        chunk_size = BUCKETS_MIN_CHUNK_SIZE;
    }
    if (chunk_size > BUCKETS_MAX_CHUNK_SIZE) {
        chunk_size = BUCKETS_MAX_CHUNK_SIZE;
    }

    return chunk_size;
}

/* Check if object should be inlined */
bool buckets_should_inline_object(size_t size)
{
    return size < BUCKETS_INLINE_THRESHOLD;
}

/* Select erasure coding configuration */
void buckets_select_erasure_config(u32 cluster_size, u32 *k, u32 *m)
{
    if (!k || !m) {
        buckets_error("NULL parameter in select_erasure_config");
        return;
    }

    /* Select based on cluster size */
    if (cluster_size >= 20) {
        *k = 16; *m = 4;  /* 25% overhead */
    } else if (cluster_size >= 16) {
        *k = 12; *m = 4;  /* 33% overhead */
    } else if (cluster_size >= 12) {
        *k = 8; *m = 4;   /* 50% overhead (default) */
    } else if (cluster_size >= 6) {
        *k = 4; *m = 2;   /* 50% overhead (minimum) */
    } else {
        /* Very small cluster - use minimal config */
        *k = 2; *m = 1;   /* 50% overhead */
    }

    buckets_debug("Selected erasure config: %u+%u for cluster size %u", 
                  *k, *m, cluster_size);
}

/* Get current time in ISO 8601 format */
void buckets_get_iso8601_time(char *buf)
{
    if (!buf) {
        return;
    }

    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    
    strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", tm_info);
}
