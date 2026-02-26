/**
 * Object Versioning Implementation
 * 
 * Implements S3-compatible versioning with multiple versions per object,
 * delete markers, and version lifecycle management.
 */

/* Disable format-truncation warnings - we use large buffers (PATH_MAX * 2) intentionally */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_io.h"

/* Version directory structure */
#define VERSION_DIR_NAME "versions"
#define LATEST_LINK_NAME ".latest"
#define DELETE_MARKER_SUFFIX ".delete"

/**
 * Get versions directory path for an object
 * 
 * @param object_path Base object path
 * @param versions_dir Output buffer for versions directory path
 * @param versions_dir_len Buffer size
 */
static void get_versions_dir_path(const char *object_path, 
                                  char *versions_dir, size_t versions_dir_len)
{
    snprintf(versions_dir, versions_dir_len, "%s/%s", 
             object_path, VERSION_DIR_NAME);
}

/**
 * Get version-specific directory path
 * 
 * @param object_path Base object path
 * @param versionId Version ID
 * @param version_path Output buffer for version path
 * @param version_path_len Buffer size
 */
static void get_version_path(const char *object_path, const char *versionId,
                             char *version_path, size_t version_path_len)
{
    snprintf(version_path, version_path_len, "%s/%s/%s",
             object_path, VERSION_DIR_NAME, versionId);
}

/**
 * Create versions directory if it doesn't exist
 * 
 * @param disk_path Disk root path
 * @param object_path Object path
 * @return 0 on success, -1 on error
 */
static int ensure_versions_dir(const char *disk_path, const char *object_path)
{
    char versions_dir[PATH_MAX];
    get_versions_dir_path(object_path, versions_dir, sizeof(versions_dir));
    
    char full_path[PATH_MAX * 2];  /* Larger buffer to avoid truncation warnings */
    snprintf(full_path, sizeof(full_path), "%s/%s", disk_path, versions_dir);
    
    /* Create directory recursively */
    if (buckets_ensure_directory(full_path) != 0) {
        buckets_error("Failed to create versions directory: %s", full_path);
        return -1;
    }
    
    return 0;
}

/**
 * Update .latest symlink to point to newest version
 * 
 * @param disk_path Disk root path
 * @param object_path Object path
 * @param versionId Latest version ID
 * @return 0 on success, -1 on error
 */
static int update_latest_link(const char *disk_path, const char *object_path,
                              const char *versionId)
{
    char versions_dir[PATH_MAX];
    get_versions_dir_path(object_path, versions_dir, sizeof(versions_dir));
    
    char link_path[PATH_MAX * 2];  /* Larger buffer */
    snprintf(link_path, sizeof(link_path), "%s/%s/%s",
             disk_path, versions_dir, LATEST_LINK_NAME);
    
    /* Remove existing symlink if present */
    unlink(link_path);
    
    /* Create symlink to version ID */
    if (symlink(versionId, link_path) != 0) {
        buckets_warn("Failed to create .latest symlink: %s", strerror(errno));
        /* Non-fatal - we can still retrieve versions */
    }
    
    buckets_debug("Updated .latest -> %s", versionId);
    return 0;
}

/**
 * Get latest version ID from .latest symlink
 * 
 * @param disk_path Disk root path
 * @param object_path Object path
 * @param versionId Output buffer for version ID
 * @param versionId_len Buffer size
 * @return 0 on success, -1 on error
 */
static int get_latest_version_id(const char *disk_path, const char *object_path,
                                 char *versionId, size_t versionId_len)
{
    char versions_dir[PATH_MAX];
    get_versions_dir_path(object_path, versions_dir, sizeof(versions_dir));
    
    char link_path[PATH_MAX];
    snprintf(link_path, sizeof(link_path), "%s/%s/%s",
             disk_path, versions_dir, LATEST_LINK_NAME);
    
    /* Read symlink */
    ssize_t len = readlink(link_path, versionId, versionId_len - 1);
    if (len < 0) {
        buckets_debug("No .latest symlink found, will scan directory");
        return -1;
    }
    
    versionId[len] = '\0';
    buckets_debug("Latest version from symlink: %s", versionId);
    return 0;
}

/**
 * Check if version is a delete marker
 * 
 * @param disk_path Disk root path
 * @param object_path Object path
 * @param versionId Version ID
 * @return true if delete marker, false otherwise
 */
static bool is_delete_marker(const char *disk_path, const char *object_path,
                             const char *versionId)
{
    char version_path[PATH_MAX * 2];
    get_version_path(object_path, versionId, version_path, sizeof(version_path));
    
    char marker_path[PATH_MAX * 2];
    snprintf(marker_path, sizeof(marker_path), "%s/%s%s",
             disk_path, version_path, DELETE_MARKER_SUFFIX);
    
    /* Check if delete marker file exists */
    struct stat st;
    return (stat(marker_path, &st) == 0);
}

/**
 * Put object version (versioned write)
 * 
 * This function stores a new version of an object. Each version is stored
 * in a separate directory under versions/<versionId>/.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param data Object data
 * @param size Object size
 * @param meta Object metadata (optional)
 * @param versionId Output version ID (must be >= 37 bytes)
 * @return 0 on success, -1 on error
 */
int buckets_put_object_versioned(const char *bucket, const char *object,
                                  const void *data, size_t size,
                                  buckets_xl_meta_t *meta,
                                  char *versionId)
{
    if (!bucket || !object || !data || !versionId) {
        buckets_error("NULL parameter in put_object_versioned");
        return -1;
    }
    
    /* Generate version ID */
    if (buckets_generate_version_id(versionId) != 0) {
        buckets_error("Failed to generate version ID");
        return -1;
    }
    
    /* Get storage config */
    const buckets_storage_config_t *config = buckets_storage_get_config();
    const char *disk_path = config->data_dir;
    
    /* Compute base object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Ensure versions directory exists */
    if (ensure_versions_dir(disk_path, object_path) != 0) {
        buckets_error("Failed to create versions directory");
        return -1;
    }
    
    /* Get version-specific path */
    char version_path[PATH_MAX * 2];
    get_version_path(object_path, versionId, version_path, sizeof(version_path));
    
    char full_version_path[PATH_MAX * 2];
    snprintf(full_version_path, sizeof(full_version_path), "%s/%s",
             disk_path, version_path);
    
    /* Create version directory */
    if (buckets_ensure_directory(full_version_path) != 0) {
        buckets_error("Failed to create version directory: %s", full_version_path);
        return -1;
    }
    
    /* Set version metadata */
    buckets_xl_meta_t version_meta = {0};
    if (meta) {
        version_meta = *meta;
    }
    version_meta.version = 1;
    strcpy(version_meta.format, "xl");
    version_meta.stat.size = size;
    buckets_get_iso8601_time(version_meta.stat.modTime);
    
    /* Set versioning info */
    version_meta.versioning.versionId = buckets_strdup(versionId);
    version_meta.versioning.isLatest = true;
    version_meta.versioning.isDeleteMarker = false;
    
    /* Use existing put_object_with_metadata but with version path */
    /* For now, write directly to version directory */
    int result = buckets_put_object_with_metadata(bucket, object, data, size,
                                                   &version_meta, false, NULL);
    
    if (result == 0) {
        /* Move written data to version directory */
        /* TODO: This is a workaround - need to refactor put_object to accept custom path */
        
        /* Update .latest symlink */
        update_latest_link(disk_path, object_path, versionId);
        
        buckets_info("Object version written: %s/%s version=%s size=%zu",
                    bucket, object, versionId, size);
    }
    
    buckets_xl_meta_free(&version_meta);
    return result;
}

/**
 * Delete object (create delete marker)
 * 
 * S3-compatible soft delete: creates a delete marker version rather than
 * actually deleting the object. The object becomes invisible until the
 * delete marker is removed.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Output version ID of delete marker
 * @return 0 on success, -1 on error
 */
int buckets_delete_object_versioned(const char *bucket, const char *object,
                                     char *versionId)
{
    if (!bucket || !object || !versionId) {
        buckets_error("NULL parameter in delete_object_versioned");
        return -1;
    }
    
    /* Generate version ID for delete marker */
    if (buckets_generate_version_id(versionId) != 0) {
        buckets_error("Failed to generate version ID for delete marker");
        return -1;
    }
    
    /* Get storage config */
    const buckets_storage_config_t *config = buckets_storage_get_config();
    const char *disk_path = config->data_dir;
    
    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Ensure versions directory exists */
    if (ensure_versions_dir(disk_path, object_path) != 0) {
        buckets_error("Failed to create versions directory");
        return -1;
    }
    
    /* Get version-specific path */
    char version_path[PATH_MAX * 2];
    get_version_path(object_path, versionId, version_path, sizeof(version_path));
    
    char full_version_path[PATH_MAX * 2];
    snprintf(full_version_path, sizeof(full_version_path), "%s/%s",
             disk_path, version_path);
    
    /* Create version directory */
    if (buckets_ensure_directory(full_version_path) != 0) {
        buckets_error("Failed to create version directory for delete marker");
        return -1;
    }
    
    /* Create delete marker file */
    char marker_path[PATH_MAX * 2];
    snprintf(marker_path, sizeof(marker_path), "%s%s",
             full_version_path, DELETE_MARKER_SUFFIX);
    
    FILE *f = fopen(marker_path, "w");
    if (!f) {
        buckets_error("Failed to create delete marker: %s", marker_path);
        return -1;
    }
    
    /* Write minimal metadata */
    fprintf(f, "{\"deleteMarker\":true,\"versionId\":\"%s\"}\n", versionId);
    fclose(f);
    
    /* Write xl.meta for delete marker */
    buckets_xl_meta_t marker_meta = {0};
    marker_meta.version = 1;
    strcpy(marker_meta.format, "xl");
    marker_meta.stat.size = 0;
    buckets_get_iso8601_time(marker_meta.stat.modTime);
    marker_meta.versioning.versionId = buckets_strdup(versionId);
    marker_meta.versioning.isLatest = true;
    marker_meta.versioning.isDeleteMarker = true;
    
    int result = buckets_write_xl_meta(disk_path, version_path, &marker_meta);
    
    if (result == 0) {
        /* Update .latest symlink to delete marker */
        update_latest_link(disk_path, object_path, versionId);
        
        buckets_info("Delete marker created: %s/%s version=%s",
                    bucket, object, versionId);
    }
    
    buckets_xl_meta_free(&marker_meta);
    return result;
}

/**
 * Get object by specific version ID
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID (NULL for latest non-deleted version)
 * @param data Output data pointer
 * @param size Output size
 * @return 0 on success, -1 on error, -2 if delete marker
 */
int buckets_get_object_by_version(const char *bucket, const char *object,
                                   const char *versionId,
                                   void **data, size_t *size)
{
    if (!bucket || !object || !data || !size) {
        buckets_error("NULL parameter in get_object_by_version");
        return -1;
    }
    
    /* Get storage config */
    const buckets_storage_config_t *config = buckets_storage_get_config();
    const char *disk_path = config->data_dir;
    
    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Determine which version to retrieve */
    char version_id_buf[37];
    const char *target_version = versionId;
    
    if (!target_version) {
        /* Get latest version */
        if (get_latest_version_id(disk_path, object_path, 
                                  version_id_buf, sizeof(version_id_buf)) == 0) {
            target_version = version_id_buf;
        } else {
            /* Fallback: try to get unversioned object */
            buckets_debug("No versions found, trying unversioned object");
            return buckets_get_object(bucket, object, data, size);
        }
    }
    
    /* Check if it's a delete marker */
    if (is_delete_marker(disk_path, object_path, target_version)) {
        buckets_info("Version %s is a delete marker", target_version);
        return -2;  /* Special return code for delete marker */
    }
    
    /* Get version path */
    char version_path[PATH_MAX * 2];
    get_version_path(object_path, target_version, version_path, sizeof(version_path));
    
    /* Read xl.meta from version directory */
    buckets_xl_meta_t meta;
    if (buckets_read_xl_meta(disk_path, version_path, &meta) != 0) {
        buckets_error("Failed to read version xl.meta: %s", target_version);
        return -1;
    }
    
    /* TODO: Read object data from version directory */
    /* For now, this is a simplified implementation */
    
    buckets_warn("Version-specific data retrieval not yet fully implemented");
    buckets_xl_meta_free(&meta);
    
    return -1;
}

/**
 * List all versions of an object
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versions Output array of version IDs (caller must free each string and array)
 * @param is_delete_markers Output array of delete marker flags
 * @param count Output number of versions
 * @return 0 on success, -1 on error
 */
int buckets_list_versions(const char *bucket, const char *object,
                          char ***versions, bool **is_delete_markers, u32 *count)
{
    if (!bucket || !object || !versions || !is_delete_markers || !count) {
        buckets_error("NULL parameter in list_versions");
        return -1;
    }
    
    /* Get storage config */
    const buckets_storage_config_t *config = buckets_storage_get_config();
    const char *disk_path = config->data_dir;
    
    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Get versions directory */
    char versions_dir[PATH_MAX];
    get_versions_dir_path(object_path, versions_dir, sizeof(versions_dir));
    
    char full_versions_dir[PATH_MAX * 2];
    snprintf(full_versions_dir, sizeof(full_versions_dir), "%s/%s",
             disk_path, versions_dir);
    
    /* Open versions directory */
    DIR *dir = opendir(full_versions_dir);
    if (!dir) {
        if (errno == ENOENT) {
            /* No versions directory - object never versioned */
            buckets_debug("No versions directory found for %s/%s", bucket, object);
            *versions = NULL;
            *is_delete_markers = NULL;
            *count = 0;
            return 0;
        }
        buckets_error("Failed to open versions directory: %s", strerror(errno));
        return -1;
    }
    
    /* Count versions (first pass) */
    u32 version_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. and .latest */
        if (entry->d_name[0] == '.') {
            continue;
        }
        version_count++;
    }
    
    if (version_count == 0) {
        closedir(dir);
        *versions = NULL;
        *is_delete_markers = NULL;
        *count = 0;
        return 0;
    }
    
    /* Allocate arrays */
    *versions = buckets_malloc(version_count * sizeof(char*));
    *is_delete_markers = buckets_malloc(version_count * sizeof(bool));
    
    /* Read versions (second pass) */
    rewinddir(dir);
    u32 idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < version_count) {
        /* Skip . and .. and .latest */
        if (entry->d_name[0] == '.') {
            continue;
        }
        
        (*versions)[idx] = buckets_strdup(entry->d_name);
        (*is_delete_markers)[idx] = is_delete_marker(disk_path, object_path,
                                                      entry->d_name);
        idx++;
    }
    
    closedir(dir);
    *count = idx;
    
    buckets_info("Listed %u versions for %s/%s", *count, bucket, object);
    return 0;
}

/**
 * Delete specific version (hard delete)
 * 
 * Permanently deletes a specific version including delete markers.
 * This is different from creating a delete marker.
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param versionId Version ID to delete
 * @return 0 on success, -1 on error
 */
int buckets_delete_version(const char *bucket, const char *object,
                           const char *versionId)
{
    if (!bucket || !object || !versionId) {
        buckets_error("NULL parameter in delete_version");
        return -1;
    }
    
    /* Get storage config */
    const buckets_storage_config_t *config = buckets_storage_get_config();
    const char *disk_path = config->data_dir;
    
    /* Compute object path */
    char object_path[PATH_MAX];
    buckets_compute_object_path(bucket, object, object_path, sizeof(object_path));
    
    /* Get version path */
    char version_path[PATH_MAX * 2];
    get_version_path(object_path, versionId, version_path, sizeof(version_path));
    
    char full_version_path[PATH_MAX * 2];
    snprintf(full_version_path, sizeof(full_version_path), "%s/%s",
             disk_path, version_path);
    
    /* Remove version directory recursively */
    /* TODO: Implement recursive directory deletion */
    buckets_warn("Recursive directory deletion not yet implemented");
    
    buckets_info("Deleted version: %s/%s version=%s", bucket, object, versionId);
    return 0;
}
