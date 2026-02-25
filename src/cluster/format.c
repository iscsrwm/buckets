/**
 * Format Management Implementation
 * 
 * Handles format.json creation, serialization, validation, and persistence.
 * Format metadata defines the immutable cluster identity including:
 * - Deployment ID (cluster UUID)
 * - Erasure set topology (which disks belong to which sets)
 * - Distribution algorithm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_io.h"
#include "buckets_json.h"

/* Format version constants */
#define BUCKETS_FORMAT_VERSION "1"
#define BUCKETS_FORMAT_TYPE "erasure"
#define BUCKETS_ERASURE_VERSION "3"
#define BUCKETS_DISTRIBUTION_ALGO "SIPMOD+PARITY"

/* Forward declarations for internal functions */
static cJSON* format_to_json(buckets_format_t *format);
static buckets_format_t* format_from_json(cJSON *json);

buckets_format_t* buckets_format_new(int set_count, int disks_per_set)
{
    if (set_count <= 0 || disks_per_set <= 0) {
        buckets_error("Invalid set configuration: set_count=%d, disks_per_set=%d",
                      set_count, disks_per_set);
        return NULL;
    }
    
    /* Allocate format structure */
    buckets_format_t *format = buckets_calloc(1, sizeof(buckets_format_t));
    
    /* Initialize format metadata */
    strncpy(format->meta.version, BUCKETS_FORMAT_VERSION, sizeof(format->meta.version) - 1);
    strncpy(format->meta.format_type, BUCKETS_FORMAT_TYPE, sizeof(format->meta.format_type) - 1);
    
    /* Generate deployment ID (cluster UUID) */
    buckets_uuid_generate(format->meta.deployment_id);
    
    /* Initialize erasure information */
    strncpy(format->erasure.version, BUCKETS_ERASURE_VERSION, sizeof(format->erasure.version) - 1);
    strncpy(format->erasure.distribution_algo, BUCKETS_DISTRIBUTION_ALGO,
            sizeof(format->erasure.distribution_algo) - 1);
    
    format->erasure.set_count = set_count;
    format->erasure.disks_per_set = disks_per_set;
    
    /* Allocate sets array: sets[set_count][disks_per_set] */
    format->erasure.sets = buckets_calloc(set_count, sizeof(char **));
    for (int i = 0; i < set_count; i++) {
        format->erasure.sets[i] = buckets_calloc(disks_per_set, sizeof(char *));
        for (int j = 0; j < disks_per_set; j++) {
            /* Generate UUID for each disk */
            char disk_uuid[37];
            buckets_uuid_generate(disk_uuid);
            format->erasure.sets[i][j] = buckets_strdup(disk_uuid);
        }
    }
    
    /* This disk UUID will be set when format is assigned to a specific disk */
    format->erasure.this_disk[0] = '\0';
    
    buckets_debug("Created new format: deployment_id=%s, sets=%d, disks_per_set=%d",
                  format->meta.deployment_id, set_count, disks_per_set);
    
    return format;
}

void buckets_format_free(buckets_format_t *format)
{
    if (!format) {
        return;
    }
    
    /* Free sets array */
    if (format->erasure.sets) {
        for (int i = 0; i < format->erasure.set_count; i++) {
            if (format->erasure.sets[i]) {
                for (int j = 0; j < format->erasure.disks_per_set; j++) {
                    buckets_free(format->erasure.sets[i][j]);
                }
                buckets_free(format->erasure.sets[i]);
            }
        }
        buckets_free(format->erasure.sets);
    }
    
    buckets_free(format);
}

static cJSON* format_to_json(buckets_format_t *format)
{
    if (!format) {
        return NULL;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        buckets_error("Failed to create JSON object");
        return NULL;
    }
    
    /* Add format metadata */
    buckets_json_add_string(root, "version", format->meta.version);
    buckets_json_add_string(root, "format", format->meta.format_type);
    buckets_json_add_string(root, "id", format->meta.deployment_id);
    
    /* Create erasure object */
    cJSON *erasure = cJSON_CreateObject();
    buckets_json_add_string(erasure, "version", format->erasure.version);
    buckets_json_add_string(erasure, "this", format->erasure.this_disk);
    buckets_json_add_string(erasure, "distributionAlgo", format->erasure.distribution_algo);
    
    /* Create sets array */
    cJSON *sets = cJSON_CreateArray();
    for (int i = 0; i < format->erasure.set_count; i++) {
        cJSON *set = cJSON_CreateArray();
        for (int j = 0; j < format->erasure.disks_per_set; j++) {
            cJSON_AddItemToArray(set, cJSON_CreateString(format->erasure.sets[i][j]));
        }
        cJSON_AddItemToArray(sets, set);
    }
    buckets_json_add_array(erasure, "sets", sets);
    
    /* Add erasure object to root */
    buckets_json_add_object(root, "xl", erasure);
    
    return root;
}

static buckets_format_t* format_from_json(cJSON *json)
{
    if (!json) {
        return NULL;
    }
    
    /* Allocate format structure */
    buckets_format_t *format = buckets_calloc(1, sizeof(buckets_format_t));
    
    /* Parse format metadata */
    char *version = buckets_json_get_string(json, "version", BUCKETS_FORMAT_VERSION);
    char *format_type = buckets_json_get_string(json, "format", BUCKETS_FORMAT_TYPE);
    char *deployment_id = buckets_json_get_string(json, "id", NULL);
    
    if (!deployment_id) {
        buckets_error("Missing deployment ID in format JSON");
        buckets_free(version);
        buckets_free(format_type);
        buckets_free(format);
        return NULL;
    }
    
    strncpy(format->meta.version, version, sizeof(format->meta.version) - 1);
    strncpy(format->meta.format_type, format_type, sizeof(format->meta.format_type) - 1);
    strncpy(format->meta.deployment_id, deployment_id, sizeof(format->meta.deployment_id) - 1);
    
    buckets_free(version);
    buckets_free(format_type);
    buckets_free(deployment_id);
    
    /* Parse erasure object */
    cJSON *erasure = buckets_json_get_object(json, "xl");
    if (!erasure) {
        buckets_error("Missing 'xl' object in format JSON");
        buckets_free(format);
        return NULL;
    }
    
    char *erasure_version = buckets_json_get_string(erasure, "version", BUCKETS_ERASURE_VERSION);
    char *this_disk = buckets_json_get_string(erasure, "this", "");
    char *dist_algo = buckets_json_get_string(erasure, "distributionAlgo", BUCKETS_DISTRIBUTION_ALGO);
    
    strncpy(format->erasure.version, erasure_version, sizeof(format->erasure.version) - 1);
    strncpy(format->erasure.this_disk, this_disk, sizeof(format->erasure.this_disk) - 1);
    strncpy(format->erasure.distribution_algo, dist_algo, sizeof(format->erasure.distribution_algo) - 1);
    
    buckets_free(erasure_version);
    buckets_free(this_disk);
    buckets_free(dist_algo);
    
    /* Parse sets array */
    cJSON *sets = buckets_json_get_array(erasure, "sets");
    if (!sets) {
        buckets_error("Missing 'sets' array in erasure object");
        buckets_free(format);
        return NULL;
    }
    
    format->erasure.set_count = cJSON_GetArraySize(sets);
    if (format->erasure.set_count <= 0) {
        buckets_error("Invalid set count: %d", format->erasure.set_count);
        buckets_free(format);
        return NULL;
    }
    
    /* Allocate sets array */
    format->erasure.sets = buckets_calloc(format->erasure.set_count, sizeof(char **));
    
    /* Parse each set */
    cJSON *set = NULL;
    int set_idx = 0;
    cJSON_ArrayForEach(set, sets) {
        if (!cJSON_IsArray(set)) {
            buckets_error("Set %d is not an array", set_idx);
            buckets_format_free(format);
            return NULL;
        }
        
        int disks_in_set = cJSON_GetArraySize(set);
        if (set_idx == 0) {
            format->erasure.disks_per_set = disks_in_set;
        } else if (disks_in_set != format->erasure.disks_per_set) {
            buckets_error("Inconsistent disks per set: set 0 has %d, set %d has %d",
                          format->erasure.disks_per_set, set_idx, disks_in_set);
            buckets_format_free(format);
            return NULL;
        }
        
        /* Allocate disk array for this set */
        format->erasure.sets[set_idx] = buckets_calloc(disks_in_set, sizeof(char *));
        
        /* Parse each disk UUID */
        cJSON *disk = NULL;
        int disk_idx = 0;
        cJSON_ArrayForEach(disk, set) {
            if (!cJSON_IsString(disk)) {
                buckets_error("Disk UUID is not a string at set %d, disk %d", set_idx, disk_idx);
                buckets_format_free(format);
                return NULL;
            }
            format->erasure.sets[set_idx][disk_idx] = buckets_strdup(disk->valuestring);
            disk_idx++;
        }
        
        set_idx++;
    }
    
    buckets_debug("Parsed format from JSON: deployment_id=%s, sets=%d, disks_per_set=%d",
                  format->meta.deployment_id, format->erasure.set_count, format->erasure.disks_per_set);
    
    return format;
}

int buckets_format_save(const char *disk_path, buckets_format_t *format)
{
    if (!disk_path || !format) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Convert format to JSON */
    cJSON *json = format_to_json(format);
    if (!json) {
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Get format file path */
    char *format_path = buckets_get_format_path(disk_path);
    if (!format_path) {
        cJSON_Delete(json);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Save JSON to disk atomically */
    int ret = buckets_json_save(format_path, json, true);
    
    cJSON_Delete(json);
    buckets_free(format_path);
    
    if (ret == BUCKETS_OK) {
        buckets_info("Saved format to disk: %s (deployment_id=%s)",
                     disk_path, format->meta.deployment_id);
    } else {
        buckets_error("Failed to save format to disk: %s", disk_path);
    }
    
    return ret;
}

buckets_format_t* buckets_format_load(const char *disk_path)
{
    if (!disk_path) {
        return NULL;
    }
    
    /* Get format file path */
    char *format_path = buckets_get_format_path(disk_path);
    if (!format_path) {
        return NULL;
    }
    
    /* Load JSON from disk */
    cJSON *json = buckets_json_load(format_path);
    buckets_free(format_path);
    
    if (!json) {
        buckets_error("Failed to load format from disk: %s", disk_path);
        return NULL;
    }
    
    /* Parse JSON to format structure */
    buckets_format_t *format = format_from_json(json);
    cJSON_Delete(json);
    
    if (format) {
        buckets_debug("Loaded format from disk: %s (deployment_id=%s)",
                      disk_path, format->meta.deployment_id);
    }
    
    return format;
}

buckets_format_t* buckets_format_clone(buckets_format_t *format)
{
    if (!format) {
        return NULL;
    }
    
    /* Convert to JSON and back to create a deep copy */
    cJSON *json = format_to_json(format);
    if (!json) {
        return NULL;
    }
    
    buckets_format_t *clone = format_from_json(json);
    cJSON_Delete(json);
    
    return clone;
}

int buckets_format_validate(buckets_format_t **formats, int count)
{
    if (!formats || count <= 0) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Need at least one format */
    if (count == 0) {
        buckets_error("No formats provided for validation");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Find first non-NULL format as reference */
    buckets_format_t *reference = NULL;
    int reference_idx = -1;
    for (int i = 0; i < count; i++) {
        if (formats[i]) {
            reference = formats[i];
            reference_idx = i;
            break;
        }
    }
    
    if (!reference) {
        buckets_error("All formats are NULL");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    buckets_debug("Validating %d formats against reference (index %d)", count, reference_idx);
    
    /* Validate all formats against reference */
    int valid_count = 0;
    int mismatch_count = 0;
    
    for (int i = 0; i < count; i++) {
        if (!formats[i]) {
            buckets_warn("Format at index %d is NULL", i);
            continue;
        }
        
        /* Check deployment ID consistency */
        if (strcmp(formats[i]->meta.deployment_id, reference->meta.deployment_id) != 0) {
            buckets_error("Format %d has mismatched deployment ID: %s != %s",
                          i, formats[i]->meta.deployment_id, reference->meta.deployment_id);
            mismatch_count++;
            continue;
        }
        
        /* Check format version */
        if (strcmp(formats[i]->meta.version, reference->meta.version) != 0) {
            buckets_warn("Format %d has different version: %s != %s",
                         i, formats[i]->meta.version, reference->meta.version);
        }
        
        /* Check set topology */
        if (formats[i]->erasure.set_count != reference->erasure.set_count) {
            buckets_error("Format %d has mismatched set count: %d != %d",
                          i, formats[i]->erasure.set_count, reference->erasure.set_count);
            mismatch_count++;
            continue;
        }
        
        if (formats[i]->erasure.disks_per_set != reference->erasure.disks_per_set) {
            buckets_error("Format %d has mismatched disks per set: %d != %d",
                          i, formats[i]->erasure.disks_per_set, reference->erasure.disks_per_set);
            mismatch_count++;
            continue;
        }
        
        /* Check distribution algorithm */
        if (strcmp(formats[i]->erasure.distribution_algo, reference->erasure.distribution_algo) != 0) {
            buckets_error("Format %d has mismatched distribution algorithm: %s != %s",
                          i, formats[i]->erasure.distribution_algo, reference->erasure.distribution_algo);
            mismatch_count++;
            continue;
        }
        
        valid_count++;
    }
    
    buckets_info("Format validation: %d valid, %d mismatched, %d NULL out of %d total",
                 valid_count, mismatch_count, count - valid_count - mismatch_count, count);
    
    /* Check if we have quorum (majority) */
    int required_quorum = (count / 2) + 1;
    if (valid_count < required_quorum) {
        buckets_error("Format validation failed: only %d valid formats, need %d for quorum",
                      valid_count, required_quorum);
        return BUCKETS_ERR_QUORUM;
    }
    
    buckets_info("Format validation successful: quorum achieved (%d/%d)",
                 valid_count, required_quorum);
    
    return BUCKETS_OK;
}
