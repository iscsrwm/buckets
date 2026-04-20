/**
 * S3 Versioning Operations
 * 
 * Implements S3-compatible versioning APIs:
 * - PUT bucket versioning (enable/suspend)
 * - GET bucket versioning (status)
 * - GET object with versionId
 * - DELETE object with versionId
 * - LIST object versions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "buckets.h"
#include "buckets_s3.h"
#include "buckets_storage.h"
#include "cJSON.h"

/* ===================================================================
 * Bucket Versioning Configuration
 * ===================================================================*/

/* Versioning status values */
#define VERSIONING_ENABLED   "Enabled"
#define VERSIONING_SUSPENDED "Suspended"

/* System bucket for storing bucket metadata */
#define BUCKETS_SYSTEM_BUCKET ".buckets.sys"

/* ===================================================================
 * Versioning Status Cache
 * 
 * Caches bucket versioning status to avoid disk reads on every request.
 * Cache is invalidated when versioning status is changed.
 * ===================================================================*/

#include <pthread.h>

#define VERSIONING_CACHE_SIZE 256

typedef struct {
    char bucket[256];
    bool enabled;
    bool suspended;
    bool valid;
    time_t cached_at;
} versioning_cache_entry_t;

static versioning_cache_entry_t g_versioning_cache[VERSIONING_CACHE_SIZE];
static pthread_rwlock_t g_versioning_cache_lock = PTHREAD_RWLOCK_INITIALIZER;
static bool g_versioning_cache_initialized = false;

static void versioning_cache_init(void)
{
    if (g_versioning_cache_initialized) return;
    memset(g_versioning_cache, 0, sizeof(g_versioning_cache));
    g_versioning_cache_initialized = true;
}

static unsigned int versioning_cache_hash(const char *bucket)
{
    unsigned int hash = 5381;
    int c;
    while ((c = *bucket++))
        hash = ((hash << 5) + hash) + c;
    return hash % VERSIONING_CACHE_SIZE;
}

static bool versioning_cache_get(const char *bucket, bool *enabled, bool *suspended)
{
    versioning_cache_init();
    
    unsigned int idx = versioning_cache_hash(bucket);
    
    pthread_rwlock_rdlock(&g_versioning_cache_lock);
    versioning_cache_entry_t *entry = &g_versioning_cache[idx];
    
    if (entry->valid && strcmp(entry->bucket, bucket) == 0) {
        *enabled = entry->enabled;
        *suspended = entry->suspended;
        pthread_rwlock_unlock(&g_versioning_cache_lock);
        return true;
    }
    pthread_rwlock_unlock(&g_versioning_cache_lock);
    return false;
}

static void versioning_cache_put(const char *bucket, bool enabled, bool suspended)
{
    versioning_cache_init();
    
    unsigned int idx = versioning_cache_hash(bucket);
    
    pthread_rwlock_wrlock(&g_versioning_cache_lock);
    versioning_cache_entry_t *entry = &g_versioning_cache[idx];
    
    strncpy(entry->bucket, bucket, sizeof(entry->bucket) - 1);
    entry->bucket[sizeof(entry->bucket) - 1] = '\0';
    entry->enabled = enabled;
    entry->suspended = suspended;
    entry->valid = true;
    entry->cached_at = time(NULL);
    
    pthread_rwlock_unlock(&g_versioning_cache_lock);
}

/* versioning_cache_invalidate is no longer used - we always update
 * the cache with the new value rather than invalidating it.
 * Keeping this as a comment in case we need it in the future.
 *
 * static void versioning_cache_invalidate(const char *bucket)
 * {
 *     versioning_cache_init();
 *     unsigned int idx = versioning_cache_hash(bucket);
 *     pthread_rwlock_wrlock(&g_versioning_cache_lock);
 *     versioning_cache_entry_t *entry = &g_versioning_cache[idx];
 *     if (strcmp(entry->bucket, bucket) == 0) {
 *         entry->valid = false;
 *     }
 *     pthread_rwlock_unlock(&g_versioning_cache_lock);
 * }
 */

/**
 * Get versioning configuration file path
 */
static void get_versioning_config_path(const char *bucket, char *path, size_t path_len)
{
    snprintf(path, path_len, "buckets/%s/versioning.json", bucket);
}

/**
 * Load bucket versioning status
 * 
 * @param bucket Bucket name
 * @param enabled Output: true if versioning is enabled
 * @param suspended Output: true if versioning was enabled but is now suspended
 * @return 0 on success, -1 on error (bucket never had versioning configured)
 */
int buckets_get_bucket_versioning(const char *bucket, bool *enabled, bool *suspended)
{
    if (!bucket || !enabled || !suspended) {
        return -1;
    }
    
    *enabled = false;
    *suspended = false;
    
    /* Check cache first - this is the fast path */
    if (versioning_cache_get(bucket, enabled, suspended)) {
        return 0;  /* Cache hit */
    }
    
    /* On cache miss, return "disabled" immediately WITHOUT doing I/O.
     * 
     * This function is called from the event loop thread during streaming uploads.
     * Doing any I/O here (like buckets_get_object) would block the event loop and
     * cause deadlock when multiple nodes are making RPC calls to each other.
     * 
     * The cache is populated when:
     * 1. PUT bucket versioning is called (which updates the cache)
     * 
     * This means:
     * - New buckets default to unversioned (correct)
     * - Buckets with versioning enabled will have their status cached after
     *   the first PUT bucket versioning call
     * - On server restart, versioned buckets will appear unversioned until
     *   a client calls PUT/GET bucket versioning (this is a known limitation)
     * 
     * TODO: Load versioning configs at startup from local disk only.
     */
    versioning_cache_put(bucket, false, false);
    return 0;
}

/**
 * Set bucket versioning status
 * 
 * @param bucket Bucket name
 * @param enabled True to enable, false to suspend
 * @return 0 on success, -1 on error
 */
int buckets_set_bucket_versioning(const char *bucket, bool enabled)
{
    if (!bucket) {
        return -1;
    }
    
    /* Build JSON config */
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }
    
    cJSON_AddStringToObject(root, "Status", 
                            enabled ? VERSIONING_ENABLED : VERSIONING_SUSPENDED);
    
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    if (!json_str) {
        return -1;
    }
    
    /* Build object key */
    char object_key[512];
    get_versioning_config_path(bucket, object_key, sizeof(object_key));
    
    /* Save to system bucket */
    extern int buckets_put_object(const char *bucket, const char *object,
                                   const void *data, size_t size,
                                   const char *content_type);
    
    int ret = buckets_put_object(BUCKETS_SYSTEM_BUCKET, object_key,
                                  json_str, strlen(json_str),
                                  "application/json");
    
    buckets_free(json_str);
    
    if (ret == 0) {
        /* Update cache with new versioning status */
        versioning_cache_put(bucket, enabled, !enabled);
        
        buckets_info("Bucket versioning %s: %s", 
                     enabled ? "enabled" : "suspended", bucket);
    }
    
    return ret;
}

/* Note: Versioning status is loaded lazily. On cache miss, we assume disabled.
 * This is correct because:
 * 1. You can't have a versioned bucket without first calling PUT versioning
 * 2. When PUT versioning is called, we update the cache
 * 3. New buckets are always unversioned by default
 * 
 * For server restart with existing versioned buckets, the cache will be populated
 * when the first PUT bucket versioning request is made (which S3 clients typically
 * do to check/set versioning status).
 */

/* ===================================================================
 * S3 API Handlers for Versioning
 * ===================================================================*/

/**
 * PUT bucket versioning
 * PUT /{bucket}?versioning
 * 
 * Request body:
 * <VersioningConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
 *   <Status>Enabled|Suspended</Status>
 * </VersioningConfiguration>
 */
int buckets_s3_put_bucket_versioning(buckets_s3_request_t *req,
                                      buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Parse request body for Status */
    bool enable = false;
    
    if (req->body && req->body_len > 0) {
        /* Simple XML parsing - look for <Status>Enabled</Status> or <Status>Suspended</Status> */
        if (strstr(req->body, "<Status>Enabled</Status>") ||
            strstr(req->body, "<Status>Enabled<")) {
            enable = true;
        } else if (strstr(req->body, "<Status>Suspended</Status>") ||
                   strstr(req->body, "<Status>Suspended<")) {
            enable = false;
        } else {
            /* Invalid status */
            buckets_s3_xml_error(res, "MalformedXML",
                                "Invalid versioning status in request body",
                                req->bucket);
            return BUCKETS_ERR_INVALID_ARG;
        }
    } else {
        buckets_s3_xml_error(res, "MalformedXML",
                            "Missing request body", req->bucket);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Set versioning status */
    int ret = buckets_set_bucket_versioning(req->bucket, enable);
    if (ret != 0) {
        buckets_s3_xml_error(res, "InternalError",
                            "Failed to set versioning status", req->bucket);
        return ret;
    }
    
    res->status_code = 200;
    return BUCKETS_OK;
}

/**
 * GET bucket versioning
 * GET /{bucket}?versioning
 * 
 * Response:
 * <VersioningConfiguration xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
 *   <Status>Enabled|Suspended</Status>
 * </VersioningConfiguration>
 */
int buckets_s3_get_bucket_versioning(buckets_s3_request_t *req,
                                      buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    bool enabled = false;
    bool suspended = false;
    
    /* Get versioning status (ignore error - just means never configured) */
    buckets_get_bucket_versioning(req->bucket, &enabled, &suspended);
    
    /* Build XML response */
    char xml[512];
    if (enabled) {
        snprintf(xml, sizeof(xml),
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<VersioningConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
                 "  <Status>Enabled</Status>\n"
                 "</VersioningConfiguration>");
    } else if (suspended) {
        snprintf(xml, sizeof(xml),
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<VersioningConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
                 "  <Status>Suspended</Status>\n"
                 "</VersioningConfiguration>");
    } else {
        /* Never configured - return empty configuration */
        snprintf(xml, sizeof(xml),
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<VersioningConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"/>");
    }
    
    res->body = buckets_strdup(xml);
    res->body_len = strlen(xml);
    res->status_code = 200;
    strncpy(res->content_type, "application/xml", sizeof(res->content_type) - 1);
    
    return BUCKETS_OK;
}

/**
 * GET object with version ID
 * GET /{bucket}/{key}?versionId={id}
 */
int buckets_s3_get_object_version(buckets_s3_request_t *req,
                                   buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Get versionId from query params */
    const char *version_id = NULL;
    for (int i = 0; i < req->query_count; i++) {
        if (req->query_params_keys[i] && 
            strcmp(req->query_params_keys[i], "versionId") == 0) {
            version_id = req->query_params_values[i];
            break;
        }
    }
    
    if (!version_id || strlen(version_id) == 0) {
        /* No version ID - fall back to regular GET */
        return buckets_s3_get_object(req, res);
    }
    
    /* Get object by version */
    void *data = NULL;
    size_t size = 0;
    
    int ret = buckets_get_object_by_version(req->bucket, req->key, version_id,
                                             &data, &size);
    
    if (ret == -2) {
        /* Delete marker - return 404 with special header */
        buckets_s3_xml_error(res, "NoSuchKey",
                            "The specified key does not exist (delete marker)",
                            req->key);
        strncpy(res->version_id, version_id, sizeof(res->version_id) - 1);
        /* S3 returns x-amz-delete-marker: true header */
        return BUCKETS_OK;
    }
    
    if (ret != 0 || !data) {
        buckets_s3_xml_error(res, "NoSuchVersion",
                            "The specified version does not exist",
                            req->key);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Success */
    res->body = data;
    res->body_len = size;
    res->status_code = 200;
    res->content_length = (i64)size;
    strncpy(res->version_id, version_id, sizeof(res->version_id) - 1);
    
    return BUCKETS_OK;
}

/**
 * DELETE object with version ID (hard delete) or without (create delete marker)
 * DELETE /{bucket}/{key}?versionId={id}  - Hard delete specific version
 * DELETE /{bucket}/{key}                  - Create delete marker (if versioning enabled)
 */
int buckets_s3_delete_object_versioned(buckets_s3_request_t *req,
                                        buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Get versionId from query params */
    const char *version_id = NULL;
    for (int i = 0; i < req->query_count; i++) {
        if (req->query_params_keys[i] && 
            strcmp(req->query_params_keys[i], "versionId") == 0) {
            version_id = req->query_params_values[i];
            break;
        }
    }
    
    if (version_id && strlen(version_id) > 0) {
        /* Hard delete specific version */
        int ret = buckets_delete_version(req->bucket, req->key, version_id);
        if (ret != 0) {
            buckets_s3_xml_error(res, "NoSuchVersion",
                                "The specified version does not exist",
                                req->key);
            return ret;
        }
        
        res->status_code = 204;
        strncpy(res->version_id, version_id, sizeof(res->version_id) - 1);
        return BUCKETS_OK;
    }
    
    /* No version ID - check if versioning is enabled */
    bool enabled = false;
    bool suspended = false;
    buckets_get_bucket_versioning(req->bucket, &enabled, &suspended);
    
    if (enabled) {
        /* Create delete marker */
        char delete_marker_version[64];
        int ret = buckets_delete_object_versioned(req->bucket, req->key,
                                                   delete_marker_version);
        if (ret != 0) {
            buckets_s3_xml_error(res, "InternalError",
                                "Failed to create delete marker",
                                req->key);
            return ret;
        }
        
        res->status_code = 204;
        snprintf(res->version_id, sizeof(res->version_id), "%s", delete_marker_version);
        /* S3 also returns x-amz-delete-marker: true header */
        return BUCKETS_OK;
    }
    
    /* Versioning not enabled - fall back to regular delete */
    return buckets_s3_delete_object(req, res);
}

/**
 * LIST object versions
 * GET /{bucket}?versions
 * 
 * Response includes all versions and delete markers for objects in the bucket.
 */
int buckets_s3_list_object_versions(buckets_s3_request_t *req,
                                     buckets_s3_response_t *res)
{
    if (!req || !res) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Get optional prefix and delimiter from query params */
    const char *prefix = NULL;
    const char *delimiter = NULL;
    const char *key_marker = NULL;
    const char *version_id_marker = NULL;
    int max_keys = 1000;
    
    for (int i = 0; i < req->query_count; i++) {
        if (!req->query_params_keys[i]) continue;
        
        if (strcmp(req->query_params_keys[i], "prefix") == 0) {
            prefix = req->query_params_values[i];
        } else if (strcmp(req->query_params_keys[i], "delimiter") == 0) {
            delimiter = req->query_params_values[i];
        } else if (strcmp(req->query_params_keys[i], "key-marker") == 0) {
            key_marker = req->query_params_values[i];
        } else if (strcmp(req->query_params_keys[i], "version-id-marker") == 0) {
            version_id_marker = req->query_params_values[i];
        } else if (strcmp(req->query_params_keys[i], "max-keys") == 0) {
            max_keys = atoi(req->query_params_values[i]);
            if (max_keys <= 0 || max_keys > 1000) max_keys = 1000;
        }
    }
    
    (void)key_marker;
    (void)version_id_marker;
    (void)delimiter;
    
    /* Start building XML response */
    size_t xml_capacity = 65536;
    char *xml = buckets_malloc(xml_capacity);
    if (!xml) {
        buckets_s3_xml_error(res, "InternalError", "Out of memory", req->bucket);
        return BUCKETS_ERR_NOMEM;
    }
    
    size_t xml_len = 0;
    xml_len += snprintf(xml + xml_len, xml_capacity - xml_len,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ListVersionsResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n"
        "  <Name>%s</Name>\n"
        "  <Prefix>%s</Prefix>\n"
        "  <MaxKeys>%d</MaxKeys>\n"
        "  <IsTruncated>false</IsTruncated>\n",
        req->bucket,
        prefix ? prefix : "",
        max_keys);
    
    /* List objects from registry and get versions for each */
    /* For now, use a simplified implementation that scans the bucket */
    extern int buckets_registry_list(const char *bucket, const char *prefix,
                                      char ***keys, u32 *count);
    
    char **keys = NULL;
    u32 key_count = 0;
    
    int ret = buckets_registry_list(req->bucket, prefix, &keys, &key_count);
    if (ret == 0 && keys && key_count > 0) {
        int versions_listed = 0;
        
        for (u32 i = 0; i < key_count && versions_listed < max_keys; i++) {
            /* Get versions for this key */
            char **versions = NULL;
            bool *is_delete_markers = NULL;
            u32 version_count = 0;
            
            ret = buckets_list_versions(req->bucket, keys[i],
                                         &versions, &is_delete_markers, &version_count);
            
            if (ret == 0 && versions && version_count > 0) {
                for (u32 v = 0; v < version_count && versions_listed < max_keys; v++) {
                    if (is_delete_markers[v]) {
                        /* Delete marker */
                        xml_len += snprintf(xml + xml_len, xml_capacity - xml_len,
                            "  <DeleteMarker>\n"
                            "    <Key>%s</Key>\n"
                            "    <VersionId>%s</VersionId>\n"
                            "    <IsLatest>%s</IsLatest>\n"
                            "    <LastModified>2026-01-01T00:00:00.000Z</LastModified>\n"
                            "  </DeleteMarker>\n",
                            keys[i],
                            versions[v],
                            (v == 0) ? "true" : "false");
                    } else {
                        /* Regular version */
                        xml_len += snprintf(xml + xml_len, xml_capacity - xml_len,
                            "  <Version>\n"
                            "    <Key>%s</Key>\n"
                            "    <VersionId>%s</VersionId>\n"
                            "    <IsLatest>%s</IsLatest>\n"
                            "    <LastModified>2026-01-01T00:00:00.000Z</LastModified>\n"
                            "    <Size>0</Size>\n"
                            "    <StorageClass>STANDARD</StorageClass>\n"
                            "  </Version>\n",
                            keys[i],
                            versions[v],
                            (v == 0) ? "true" : "false");
                    }
                    versions_listed++;
                }
                
                /* Free versions */
                for (u32 v = 0; v < version_count; v++) {
                    buckets_free(versions[v]);
                }
                buckets_free(versions);
                buckets_free(is_delete_markers);
            } else if (version_count == 0) {
                /* Object exists but has no versioning - show as "null" version */
                xml_len += snprintf(xml + xml_len, xml_capacity - xml_len,
                    "  <Version>\n"
                    "    <Key>%s</Key>\n"
                    "    <VersionId>null</VersionId>\n"
                    "    <IsLatest>true</IsLatest>\n"
                    "    <LastModified>2026-01-01T00:00:00.000Z</LastModified>\n"
                    "    <Size>0</Size>\n"
                    "    <StorageClass>STANDARD</StorageClass>\n"
                    "  </Version>\n",
                    keys[i]);
                versions_listed++;
            }
        }
        
        /* Free keys */
        for (u32 i = 0; i < key_count; i++) {
            buckets_free(keys[i]);
        }
        buckets_free(keys);
    }
    
    xml_len += snprintf(xml + xml_len, xml_capacity - xml_len,
        "</ListVersionsResult>");
    
    res->body = xml;
    res->body_len = xml_len;
    res->status_code = 200;
    strncpy(res->content_type, "application/xml", sizeof(res->content_type) - 1);
    
    return BUCKETS_OK;
}

/**
 * Check if request has versionId query parameter
 */
bool buckets_s3_has_version_id(buckets_s3_request_t *req)
{
    if (!req) return false;
    
    for (int i = 0; i < req->query_count; i++) {
        if (req->query_params_keys[i] && 
            strcmp(req->query_params_keys[i], "versionId") == 0 &&
            req->query_params_values[i] &&
            strlen(req->query_params_values[i]) > 0) {
            return true;
        }
    }
    return false;
}

/**
 * Check if request is for bucket versioning configuration
 */
bool buckets_s3_is_versioning_request(buckets_s3_request_t *req)
{
    if (!req) return false;
    
    for (int i = 0; i < req->query_count; i++) {
        if (req->query_params_keys[i] && 
            strcmp(req->query_params_keys[i], "versioning") == 0) {
            return true;
        }
    }
    return false;
}

/**
 * Check if request is for list versions
 */
bool buckets_s3_is_list_versions_request(buckets_s3_request_t *req)
{
    if (!req) return false;
    
    for (int i = 0; i < req->query_count; i++) {
        if (req->query_params_keys[i] && 
            strcmp(req->query_params_keys[i], "versions") == 0) {
            return true;
        }
    }
    return false;
}
