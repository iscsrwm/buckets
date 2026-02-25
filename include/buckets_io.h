/**
 * Buckets I/O Utilities
 * 
 * Atomic file operations and disk utilities
 */

#ifndef BUCKETS_IO_H
#define BUCKETS_IO_H

#include "buckets.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Atomic File Operations ===== */

/**
 * Atomically write data to a file using temp file + rename pattern
 * 
 * @param path Target file path
 * @param data Data to write
 * @param size Size of data in bytes
 * @return BUCKETS_OK on success, error code on failure
 */
int buckets_atomic_write(const char *path, const void *data, size_t size);

/**
 * Atomically read entire file into memory
 * 
 * @param path File path
 * @param data_out Output pointer (caller must free with buckets_free)
 * @param size_out Output size in bytes
 * @return BUCKETS_OK on success, error code on failure
 */
int buckets_atomic_read(const char *path, void **data_out, size_t *size_out);

/**
 * Ensure directory exists (create with parents if needed)
 * 
 * @param path Directory path
 * @return BUCKETS_OK on success, error code on failure
 */
int buckets_ensure_directory(const char *path);

/**
 * Sync directory to ensure metadata is persisted
 * 
 * @param path Directory path
 * @return BUCKETS_OK on success, error code on failure
 */
int buckets_sync_directory(const char *path);

/* ===== Disk Path Utilities ===== */

/**
 * Get the metadata directory path for a disk
 * Returns: <disk-path>/.buckets.sys
 * 
 * @param disk_path Disk mount path
 * @return Allocated string (caller must free), or NULL on error
 */
char* buckets_get_meta_dir(const char *disk_path);

/**
 * Get the format.json path for a disk
 * Returns: <disk-path>/.buckets.sys/format.json
 * 
 * @param disk_path Disk mount path
 * @return Allocated string (caller must free), or NULL on error
 */
char* buckets_get_format_path(const char *disk_path);

/**
 * Get the topology.json path for a disk
 * Returns: <disk-path>/.buckets.sys/topology.json
 * 
 * @param disk_path Disk mount path
 * @return Allocated string (caller must free), or NULL on error
 */
char* buckets_get_topology_path(const char *disk_path);

/**
 * Check if a disk has valid format metadata
 * 
 * @param disk_path Disk mount path
 * @return true if format.json exists and is readable
 */
bool buckets_disk_is_formatted(const char *disk_path);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_IO_H */
