/**
 * JSON Helpers Implementation
 * 
 * Wrapper functions around cJSON for common operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buckets.h"
#include "buckets_io.h"
#include "buckets_json.h"

cJSON* buckets_json_parse(const char *json_str)
{
    if (!json_str) {
        return NULL;
    }
    
    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            buckets_error("JSON parse error near: %s", error_ptr);
        } else {
            buckets_error("JSON parse error");
        }
        return NULL;
    }
    
    return json;
}

cJSON* buckets_json_load(const char *path)
{
    if (!path) {
        return NULL;
    }
    
    /* Read file */
    void *data = NULL;
    size_t size = 0;
    int ret = buckets_atomic_read(path, &data, &size);
    
    if (ret != BUCKETS_OK) {
        return NULL;
    }
    
    /* Parse JSON */
    cJSON *json = buckets_json_parse((const char *)data);
    buckets_free(data);
    
    return json;
}

int buckets_json_save(const char *path, cJSON *json, bool formatted)
{
    if (!path || !json) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Serialize JSON */
    char *json_str = formatted ? cJSON_Print(json) : cJSON_PrintUnformatted(json);
    if (!json_str) {
        buckets_error("Failed to serialize JSON");
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Write atomically */
    size_t len = strlen(json_str);
    int ret = buckets_atomic_write(path, json_str, len);
    
    /* cJSON uses its own malloc, so we use free() directly */
    free(json_str);
    
    return ret;
}

char* buckets_json_get_string(cJSON *obj, const char *key, const char *default_value)
{
    if (!obj || !key) {
        return default_value ? buckets_strdup(default_value) : NULL;
    }
    
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item || !cJSON_IsString(item)) {
        return default_value ? buckets_strdup(default_value) : NULL;
    }
    
    return buckets_strdup(item->valuestring);
}

int buckets_json_get_int(cJSON *obj, const char *key, int default_value)
{
    if (!obj || !key) {
        return default_value;
    }
    
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item || !cJSON_IsNumber(item)) {
        return default_value;
    }
    
    return item->valueint;
}

bool buckets_json_get_bool(cJSON *obj, const char *key, bool default_value)
{
    if (!obj || !key) {
        return default_value;
    }
    
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item || !cJSON_IsBool(item)) {
        return default_value;
    }
    
    return cJSON_IsTrue(item);
}

cJSON* buckets_json_get_object(cJSON *obj, const char *key)
{
    if (!obj || !key) {
        return NULL;
    }
    
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item || !cJSON_IsObject(item)) {
        return NULL;
    }
    
    return item;
}

cJSON* buckets_json_get_array(cJSON *obj, const char *key)
{
    if (!obj || !key) {
        return NULL;
    }
    
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item || !cJSON_IsArray(item)) {
        return NULL;
    }
    
    return item;
}

void buckets_json_add_string(cJSON *obj, const char *key, const char *value)
{
    if (!obj || !key || !value) {
        return;
    }
    
    cJSON_AddStringToObject(obj, key, value);
}

void buckets_json_add_int(cJSON *obj, const char *key, int value)
{
    if (!obj || !key) {
        return;
    }
    
    cJSON_AddNumberToObject(obj, key, value);
}

void buckets_json_add_bool(cJSON *obj, const char *key, bool value)
{
    if (!obj || !key) {
        return;
    }
    
    cJSON_AddBoolToObject(obj, key, value);
}

void buckets_json_add_object(cJSON *obj, const char *key, cJSON *child)
{
    if (!obj || !key || !child) {
        return;
    }
    
    cJSON_AddItemToObject(obj, key, child);
}

void buckets_json_add_array(cJSON *obj, const char *key, cJSON *array)
{
    if (!obj || !key || !array) {
        return;
    }
    
    cJSON_AddItemToObject(obj, key, array);
}
