/**
 * Chunk I/O Implementation
 * 
 * Chunk read/write operations with checksum verification.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_crypto.h"
#include "buckets_io.h"

/* Read chunk from disk */
int buckets_read_chunk(const char *disk_path, const char *object_path,
                       u32 chunk_index, void **data, size_t *size)
{
    if (!disk_path || !object_path || !data || !size) {
        buckets_error("NULL parameter in read_chunk");
        return -1;
    }

    /* Construct chunk path */
    char chunk_path[PATH_MAX];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%spart.%u",
             disk_path, object_path, chunk_index);

    /* Read chunk file */
    if (buckets_atomic_read(chunk_path, data, size) != 0) {
        buckets_error("Failed to read chunk: %s", chunk_path);
        return -1;
    }

    return 0;
}

/* Write chunk to disk (atomic) */
int buckets_write_chunk(const char *disk_path, const char *object_path,
                        u32 chunk_index, const void *data, size_t size)
{
    if (!disk_path || !object_path || !data) {
        buckets_error("NULL parameter in write_chunk");
        return -1;
    }

    /* Construct chunk path */
    char chunk_path[PATH_MAX];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%spart.%u",
             disk_path, object_path, chunk_index);

    /* Write chunk atomically */
    if (buckets_atomic_write(chunk_path, data, size) != 0) {
        buckets_error("Failed to write chunk: %s", chunk_path);
        return -1;
    }

    return 0;
}

/* Verify chunk checksum */
bool buckets_verify_chunk(const void *data, size_t size,
                          const buckets_checksum_t *checksum)
{
    if (!data || !checksum) {
        buckets_error("NULL parameter in verify_chunk");
        return false;
    }

    /* Only BLAKE2b-256 supported for now */
    if (strcmp(checksum->algo, "BLAKE2b-256") != 0) {
        buckets_error("Unsupported checksum algorithm: %s", checksum->algo);
        return false;
    }

    /* Compute BLAKE2b-256 hash */
    u8 computed[32];
    buckets_blake2b(computed, 32, data, size, NULL, 0);

    /* Constant-time verification */
    return buckets_blake2b_verify(computed, checksum->hash, 32);
}

/* Compute chunk checksum */
int buckets_compute_chunk_checksum(const void *data, size_t size,
                                   buckets_checksum_t *checksum)
{
    if (!data || !checksum) {
        buckets_error("NULL parameter in compute_chunk_checksum");
        return -1;
    }

    /* Use BLAKE2b-256 */
    strcpy(checksum->algo, "BLAKE2b-256");
    buckets_blake2b(checksum->hash, 32, data, size, NULL, 0);

    return 0;
}

/* Delete chunk from disk */
int buckets_delete_chunk(const char *disk_path, const char *object_path,
                         u32 chunk_index)
{
    if (!disk_path || !object_path) {
        buckets_error("NULL parameter in delete_chunk");
        return -1;
    }

    /* Construct chunk path */
    char chunk_path[PATH_MAX];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%spart.%u",
             disk_path, object_path, chunk_index);

    /* Delete chunk file */
    if (unlink(chunk_path) != 0) {
        buckets_error("Failed to delete chunk: %s", chunk_path);
        return -1;
    }

    return 0;
}

/* Check if chunk exists */
bool buckets_chunk_exists(const char *disk_path, const char *object_path,
                          u32 chunk_index)
{
    if (!disk_path || !object_path) {
        return false;
    }

    /* Construct chunk path */
    char chunk_path[PATH_MAX];
    snprintf(chunk_path, sizeof(chunk_path), "%s/%spart.%u",
             disk_path, object_path, chunk_index);

    /* Check if file exists */
    return (access(chunk_path, F_OK) == 0);
}
