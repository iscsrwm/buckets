/**
 * Buckets JSON Utilities
 * 
 * Wrapper functions around cJSON for common operations
 */

#ifndef BUCKETS_JSON_H
#define BUCKETS_JSON_H

#include "buckets.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== JSON Parsing Helpers ===== */

/**
 * Parse JSON string into cJSON object
 * 
 * @param json_str JSON string
 * @return cJSON object (caller must call cJSON_Delete), or NULL on error
 */
cJSON* buckets_json_parse(const char *json_str);

/**
 * Load JSON from file
 * 
 * @param path File path
 * @return cJSON object (caller must call cJSON_Delete), or NULL on error
 */
cJSON* buckets_json_load(const char *path);

/**
 * Save JSON to file (atomically)
 * 
 * @param path File path
 * @param json cJSON object
 * @param formatted true to pretty-print, false for compact
 * @return BUCKETS_OK on success, error code on failure
 */
int buckets_json_save(const char *path, cJSON *json, bool formatted);

/* ===== JSON Field Accessors ===== */

/**
 * Get string field from JSON object (returns copy)
 * 
 * @param obj JSON object
 * @param key Field name
 * @param default_value Default if field not found (can be NULL)
 * @return Allocated string (caller must free), or NULL if not found and no default
 */
char* buckets_json_get_string(cJSON *obj, const char *key, const char *default_value);

/**
 * Get integer field from JSON object
 * 
 * @param obj JSON object
 * @param key Field name
 * @param default_value Default if field not found
 * @return Integer value
 */
int buckets_json_get_int(cJSON *obj, const char *key, int default_value);

/**
 * Get boolean field from JSON object
 * 
 * @param obj JSON object
 * @param key Field name
 * @param default_value Default if field not found
 * @return Boolean value
 */
bool buckets_json_get_bool(cJSON *obj, const char *key, bool default_value);

/**
 * Get object field from JSON object
 * 
 * @param obj JSON object
 * @param key Field name
 * @return cJSON object (do NOT delete), or NULL if not found
 */
cJSON* buckets_json_get_object(cJSON *obj, const char *key);

/**
 * Get array field from JSON object
 * 
 * @param obj JSON object
 * @param key Field name
 * @return cJSON array (do NOT delete), or NULL if not found
 */
cJSON* buckets_json_get_array(cJSON *obj, const char *key);

/* ===== JSON Field Setters ===== */

/**
 * Add string field to JSON object
 * 
 * @param obj JSON object
 * @param key Field name
 * @param value String value
 */
void buckets_json_add_string(cJSON *obj, const char *key, const char *value);

/**
 * Add integer field to JSON object
 * 
 * @param obj JSON object
 * @param key Field name
 * @param value Integer value
 */
void buckets_json_add_int(cJSON *obj, const char *key, int value);

/**
 * Add boolean field to JSON object
 * 
 * @param obj JSON object
 * @param key Field name
 * @param value Boolean value
 */
void buckets_json_add_bool(cJSON *obj, const char *key, bool value);

/**
 * Add object field to JSON object
 * 
 * @param obj JSON object
 * @param key Field name
 * @param child Child object (ownership transferred)
 */
void buckets_json_add_object(cJSON *obj, const char *key, cJSON *child);

/**
 * Add array field to JSON object
 * 
 * @param obj JSON object
 * @param key Field name
 * @param array Array object (ownership transferred)
 */
void buckets_json_add_array(cJSON *obj, const char *key, cJSON *array);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_JSON_H */
