/**
 * Bucket Metadata Management
 * 
 * Stores bucket metadata as objects in the system bucket (.buckets.sys)
 * following MinIO's design pattern.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "cJSON.h"

/**
 * Bucket metadata structure
 */
typedef struct {
    char name[256];
    time_t created;
    bool versioning_enabled;
    bool lock_enabled;
} buckets_bucket_metadata_t;

/**
 * Create bucket metadata object
 */
static int create_bucket_metadata(const char *bucket_name, 
                                   buckets_bucket_metadata_t *metadata)
{
    if (!bucket_name || !metadata) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    memset(metadata, 0, sizeof(buckets_bucket_metadata_t));
    strncpy(metadata->name, bucket_name, sizeof(metadata->name) - 1);
    metadata->created = time(NULL);
    metadata->versioning_enabled = false;
    metadata->lock_enabled = false;
    
    return BUCKETS_OK;
}

/**
 * Serialize bucket metadata to JSON
 */
static char* serialize_bucket_metadata(const buckets_bucket_metadata_t *metadata)
{
    if (!metadata) {
        return NULL;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    
    cJSON_AddStringToObject(root, "name", metadata->name);
    cJSON_AddNumberToObject(root, "created", (double)metadata->created);
    cJSON_AddBoolToObject(root, "versioning_enabled", metadata->versioning_enabled);
    cJSON_AddBoolToObject(root, "lock_enabled", metadata->lock_enabled);
    
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    
    return json_str;
}

/**
 * Deserialize bucket metadata from JSON
 */
static int deserialize_bucket_metadata(const char *json_str,
                                        buckets_bucket_metadata_t *metadata)
{
    if (!json_str || !metadata) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        buckets_error("Failed to parse bucket metadata JSON");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *created = cJSON_GetObjectItem(root, "created");
    cJSON *versioning = cJSON_GetObjectItem(root, "versioning_enabled");
    cJSON *lock = cJSON_GetObjectItem(root, "lock_enabled");
    
    if (!name || !cJSON_IsString(name)) {
        cJSON_Delete(root);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    memset(metadata, 0, sizeof(buckets_bucket_metadata_t));
    strncpy(metadata->name, name->valuestring, sizeof(metadata->name) - 1);
    
    if (created && cJSON_IsNumber(created)) {
        metadata->created = (time_t)created->valuedouble;
    }
    
    if (versioning && cJSON_IsBool(versioning)) {
        metadata->versioning_enabled = cJSON_IsTrue(versioning);
    }
    
    if (lock && cJSON_IsBool(lock)) {
        metadata->lock_enabled = cJSON_IsTrue(lock);
    }
    
    cJSON_Delete(root);
    return BUCKETS_OK;
}

/**
 * Ensure system bucket exists
 */
__attribute__((unused))
static int ensure_system_bucket(void)
{
    /* The system bucket directory should be created during initialization */
    /* For now, we'll rely on the PUT object to create it automatically */
    return BUCKETS_OK;
}

/**
 * Save bucket metadata as an object in .buckets.sys
 */
int buckets_save_bucket_metadata(const char *bucket_name)
{
    if (!bucket_name) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Create metadata structure */
    buckets_bucket_metadata_t metadata;
    int ret = create_bucket_metadata(bucket_name, &metadata);
    if (ret != BUCKETS_OK) {
        return ret;
    }
    
    /* Serialize to JSON */
    char *json_str = serialize_bucket_metadata(&metadata);
    if (!json_str) {
        buckets_error("Failed to serialize bucket metadata for %s", bucket_name);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Build object key: .buckets.sys/buckets/{bucket_name}/bucket.json */
    char object_key[512];
    snprintf(object_key, sizeof(object_key), "%s%s/%s",
             BUCKETS_BUCKET_METADATA_PREFIX, bucket_name, BUCKETS_BUCKET_METADATA_FILE);
    
    /* Save as object using PutObject */
    extern int buckets_put_object(const char *bucket, const char *object,
                                   const void *data, size_t size,
                                   const char *content_type);
    
    size_t json_len = strlen(json_str);
    ret = buckets_put_object(BUCKETS_SYSTEM_BUCKET, object_key, json_str, json_len,
                             "application/json");
    
    buckets_free(json_str);
    
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to save bucket metadata for %s", bucket_name);
        return ret;
    }
    
    buckets_info("Saved bucket metadata: %s (as object in %s)", 
                 bucket_name, BUCKETS_SYSTEM_BUCKET);
    
    return BUCKETS_OK;
}

/**
 * Load bucket metadata from .buckets.sys
 */
int buckets_load_bucket_metadata(const char *bucket_name,
                                  buckets_bucket_metadata_t *metadata)
{
    if (!bucket_name || !metadata) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Build object key */
    char object_key[512];
    snprintf(object_key, sizeof(object_key), "%s%s/%s",
             BUCKETS_BUCKET_METADATA_PREFIX, bucket_name, BUCKETS_BUCKET_METADATA_FILE);
    
    /* Load object using GetObject */
    extern int buckets_get_object(const char *bucket, const char *object,
                                   void **data, size_t *size);
    
    void *data = NULL;
    size_t size = 0;
    int ret = buckets_get_object(BUCKETS_SYSTEM_BUCKET, object_key, &data, &size);
    
    if (ret != BUCKETS_OK) {
        buckets_debug("Bucket metadata not found for %s", bucket_name);
        return ret;
    }
    
    /* Deserialize JSON */
    ret = deserialize_bucket_metadata((const char*)data, metadata);
    buckets_free(data);
    
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to deserialize bucket metadata for %s", bucket_name);
        return ret;
    }
    
    return BUCKETS_OK;
}

/**
 * Check if bucket exists by checking if metadata object exists
 */
int buckets_bucket_exists(const char *bucket_name)
{
    if (!bucket_name) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    buckets_bucket_metadata_t metadata;
    int ret = buckets_load_bucket_metadata(bucket_name, &metadata);
    
    return (ret == BUCKETS_OK) ? 1 : 0;
}

/**
 * Delete bucket metadata from .buckets.sys
 */
int buckets_delete_bucket_metadata(const char *bucket_name)
{
    if (!bucket_name) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Build object key */
    char object_key[512];
    snprintf(object_key, sizeof(object_key), "%s%s/%s",
             BUCKETS_BUCKET_METADATA_PREFIX, bucket_name, BUCKETS_BUCKET_METADATA_FILE);
    
    /* Delete object using DeleteObject */
    extern int buckets_delete_object(const char *bucket, const char *object);
    
    int ret = buckets_delete_object(BUCKETS_SYSTEM_BUCKET, object_key);
    
    if (ret != BUCKETS_OK) {
        buckets_error("Failed to delete bucket metadata for %s", bucket_name);
        return ret;
    }
    
    buckets_info("Deleted bucket metadata: %s", bucket_name);
    return BUCKETS_OK;
}
