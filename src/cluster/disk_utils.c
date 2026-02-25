/**
 * Disk Utilities
 * 
 * Path construction and disk metadata operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_io.h"

#define BUCKETS_META_DIR ".buckets.sys"
#define BUCKETS_FORMAT_FILE "format.json"
#define BUCKETS_TOPOLOGY_FILE "topology.json"

char* buckets_get_meta_dir(const char *disk_path)
{
    if (!disk_path) {
        return NULL;
    }
    
    /* Remove trailing slash if present */
    size_t len = strlen(disk_path);
    if (len > 0 && disk_path[len - 1] == '/') {
        char *trimmed = buckets_strdup(disk_path);
        trimmed[len - 1] = '\0';
        char *result = buckets_format("%s/%s", trimmed, BUCKETS_META_DIR);
        buckets_free(trimmed);
        return result;
    }
    
    return buckets_format("%s/%s", disk_path, BUCKETS_META_DIR);
}

char* buckets_get_format_path(const char *disk_path)
{
    if (!disk_path) {
        return NULL;
    }
    
    char *meta_dir = buckets_get_meta_dir(disk_path);
    if (!meta_dir) {
        return NULL;
    }
    
    char *format_path = buckets_format("%s/%s", meta_dir, BUCKETS_FORMAT_FILE);
    buckets_free(meta_dir);
    
    return format_path;
}

char* buckets_get_topology_path(const char *disk_path)
{
    if (!disk_path) {
        return NULL;
    }
    
    char *meta_dir = buckets_get_meta_dir(disk_path);
    if (!meta_dir) {
        return NULL;
    }
    
    char *topology_path = buckets_format("%s/%s", meta_dir, BUCKETS_TOPOLOGY_FILE);
    buckets_free(meta_dir);
    
    return topology_path;
}

bool buckets_disk_is_formatted(const char *disk_path)
{
    if (!disk_path) {
        return false;
    }
    
    char *format_path = buckets_get_format_path(disk_path);
    if (!format_path) {
        return false;
    }
    
    /* Check if format.json exists and is readable */
    bool exists = (access(format_path, R_OK) == 0);
    buckets_free(format_path);
    
    return exists;
}
