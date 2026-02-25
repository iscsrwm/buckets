/**
 * Object Metadata Implementation
 * 
 * xl.meta serialization/deserialization using JSON format.
 * Compatible with MinIO's xl.meta structure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "buckets.h"
#include "buckets_storage.h"
#include "buckets_json.h"
#include "buckets_io.h"
#include "cJSON.h"

/* Serialize xl.meta to JSON */
char* buckets_xl_meta_to_json(const buckets_xl_meta_t *meta)
{
    if (!meta) {
        buckets_error("NULL metadata in xl_meta_to_json");
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        buckets_error("Failed to create JSON root");
        return NULL;
    }

    /* Version and format */
    cJSON_AddNumberToObject(root, "version", meta->version);
    cJSON_AddStringToObject(root, "format", meta->format);

    /* Stat */
    cJSON *stat = cJSON_CreateObject();
    cJSON_AddNumberToObject(stat, "size", meta->stat.size);
    cJSON_AddStringToObject(stat, "modTime", meta->stat.modTime);
    cJSON_AddItemToObject(root, "stat", stat);

    /* Erasure */
    cJSON *erasure = cJSON_CreateObject();
    cJSON_AddStringToObject(erasure, "algorithm", meta->erasure.algorithm);
    cJSON_AddNumberToObject(erasure, "data", meta->erasure.data);
    cJSON_AddNumberToObject(erasure, "parity", meta->erasure.parity);
    cJSON_AddNumberToObject(erasure, "blockSize", meta->erasure.blockSize);
    cJSON_AddNumberToObject(erasure, "index", meta->erasure.index);

    /* Distribution array */
    cJSON *distribution = cJSON_CreateArray();
    for (u32 i = 0; i < meta->erasure.data + meta->erasure.parity; i++) {
        cJSON_AddItemToArray(distribution, cJSON_CreateNumber(meta->erasure.distribution[i]));
    }
    cJSON_AddItemToObject(erasure, "distribution", distribution);

    /* Checksums array */
    cJSON *checksums = cJSON_CreateArray();
    for (u32 i = 0; i < meta->erasure.data + meta->erasure.parity; i++) {
        cJSON *checksum = cJSON_CreateObject();
        cJSON_AddStringToObject(checksum, "algo", meta->erasure.checksums[i].algo);
        
        /* Convert hash bytes to hex string */
        char hex[65];
        for (int j = 0; j < 32; j++) {
            sprintf(hex + (j * 2), "%02x", meta->erasure.checksums[i].hash[j]);
        }
        cJSON_AddStringToObject(checksum, "hash", hex);
        
        cJSON_AddItemToArray(checksums, checksum);
    }
    cJSON_AddItemToObject(erasure, "checksums", checksums);
    cJSON_AddItemToObject(root, "erasure", erasure);

    /* Meta */
    cJSON *user_meta = cJSON_CreateObject();
    if (meta->meta.content_type) {
        cJSON_AddStringToObject(user_meta, "content-type", meta->meta.content_type);
    }
    if (meta->meta.etag) {
        cJSON_AddStringToObject(user_meta, "etag", meta->meta.etag);
    }
    
    /* User metadata */
    for (u32 i = 0; i < meta->meta.user_count; i++) {
        cJSON_AddStringToObject(user_meta, meta->meta.user_keys[i], 
                               meta->meta.user_values[i]);
    }
    cJSON_AddItemToObject(root, "meta", user_meta);

    /* Inline data (optional) */
    if (meta->inline_data) {
        cJSON_AddStringToObject(root, "inline", meta->inline_data);
    }

    /* Convert to string */
    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    return json_str;
}

/* Deserialize xl.meta from JSON */
int buckets_xl_meta_from_json(const char *json, buckets_xl_meta_t *meta)
{
    if (!json || !meta) {
        buckets_error("NULL parameter in xl_meta_from_json");
        return -1;
    }

    /* Initialize metadata */
    memset(meta, 0, sizeof(buckets_xl_meta_t));

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        buckets_error("Failed to parse JSON: %s", cJSON_GetErrorPtr());
        return -1;
    }

    /* Version and format */
    cJSON *version = cJSON_GetObjectItem(root, "version");
    if (version && cJSON_IsNumber(version)) {
        meta->version = (u32)version->valueint;
    }

    cJSON *format = cJSON_GetObjectItem(root, "format");
    if (format && cJSON_IsString(format)) {
        strncpy(meta->format, format->valuestring, sizeof(meta->format) - 1);
    }

    /* Stat */
    cJSON *stat = cJSON_GetObjectItem(root, "stat");
    if (stat) {
        cJSON *size = cJSON_GetObjectItem(stat, "size");
        if (size && cJSON_IsNumber(size)) {
            meta->stat.size = (size_t)size->valuedouble;
        }

        cJSON *modTime = cJSON_GetObjectItem(stat, "modTime");
        if (modTime && cJSON_IsString(modTime)) {
            strncpy(meta->stat.modTime, modTime->valuestring, sizeof(meta->stat.modTime) - 1);
        }
    }

    /* Erasure */
    cJSON *erasure = cJSON_GetObjectItem(root, "erasure");
    if (erasure) {
        cJSON *algorithm = cJSON_GetObjectItem(erasure, "algorithm");
        if (algorithm && cJSON_IsString(algorithm)) {
            strncpy(meta->erasure.algorithm, algorithm->valuestring, 
                   sizeof(meta->erasure.algorithm) - 1);
        }

        cJSON *data = cJSON_GetObjectItem(erasure, "data");
        if (data && cJSON_IsNumber(data)) {
            meta->erasure.data = (u32)data->valueint;
        }

        cJSON *parity = cJSON_GetObjectItem(erasure, "parity");
        if (parity && cJSON_IsNumber(parity)) {
            meta->erasure.parity = (u32)parity->valueint;
        }

        cJSON *blockSize = cJSON_GetObjectItem(erasure, "blockSize");
        if (blockSize && cJSON_IsNumber(blockSize)) {
            meta->erasure.blockSize = (size_t)blockSize->valuedouble;
        }

        cJSON *index = cJSON_GetObjectItem(erasure, "index");
        if (index && cJSON_IsNumber(index)) {
            meta->erasure.index = (u32)index->valueint;
        }

        /* Distribution array */
        cJSON *distribution = cJSON_GetObjectItem(erasure, "distribution");
        if (distribution && cJSON_IsArray(distribution)) {
            u32 count = meta->erasure.data + meta->erasure.parity;
            meta->erasure.distribution = buckets_malloc(count * sizeof(u32));
            
            cJSON *item = NULL;
            u32 i = 0;
            cJSON_ArrayForEach(item, distribution) {
                if (i < count && cJSON_IsNumber(item)) {
                    meta->erasure.distribution[i] = (u32)item->valueint;
                    i++;
                }
            }
        }

        /* Checksums array */
        cJSON *checksums = cJSON_GetObjectItem(erasure, "checksums");
        if (checksums && cJSON_IsArray(checksums)) {
            u32 count = meta->erasure.data + meta->erasure.parity;
            meta->erasure.checksums = buckets_malloc(count * sizeof(buckets_checksum_t));
            
            cJSON *item = NULL;
            u32 i = 0;
            cJSON_ArrayForEach(item, checksums) {
                if (i < count) {
                    cJSON *algo = cJSON_GetObjectItem(item, "algo");
                    if (algo && cJSON_IsString(algo)) {
                        strncpy(meta->erasure.checksums[i].algo, algo->valuestring,
                               sizeof(meta->erasure.checksums[i].algo) - 1);
                    }

                    cJSON *hash = cJSON_GetObjectItem(item, "hash");
                    if (hash && cJSON_IsString(hash)) {
                        /* Convert hex string to bytes */
                        const char *hex = hash->valuestring;
                        for (int j = 0; j < 32 && hex[j*2]; j++) {
                            sscanf(hex + (j * 2), "%2hhx", &meta->erasure.checksums[i].hash[j]);
                        }
                    }
                    i++;
                }
            }
        }
    }

    /* Meta */
    cJSON *user_meta = cJSON_GetObjectItem(root, "meta");
    if (user_meta) {
        cJSON *content_type = cJSON_GetObjectItem(user_meta, "content-type");
        if (content_type && cJSON_IsString(content_type)) {
            meta->meta.content_type = buckets_strdup(content_type->valuestring);
        }

        cJSON *etag = cJSON_GetObjectItem(user_meta, "etag");
        if (etag && cJSON_IsString(etag)) {
            meta->meta.etag = buckets_strdup(etag->valuestring);
        }

        /* Count user metadata items */
        meta->meta.user_count = 0;
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, user_meta) {
            if (strcmp(item->string, "content-type") != 0 && 
                strcmp(item->string, "etag") != 0) {
                meta->meta.user_count++;
            }
        }

        /* Allocate and fill user metadata */
        if (meta->meta.user_count > 0) {
            meta->meta.user_keys = buckets_malloc(meta->meta.user_count * sizeof(char*));
            meta->meta.user_values = buckets_malloc(meta->meta.user_count * sizeof(char*));
            
            u32 idx = 0;
            cJSON_ArrayForEach(item, user_meta) {
                if (strcmp(item->string, "content-type") != 0 && 
                    strcmp(item->string, "etag") != 0 && 
                    cJSON_IsString(item)) {
                    meta->meta.user_keys[idx] = buckets_strdup(item->string);
                    meta->meta.user_values[idx] = buckets_strdup(item->valuestring);
                    idx++;
                }
            }
        }
    }

    /* Inline data */
    cJSON *inline_data = cJSON_GetObjectItem(root, "inline");
    if (inline_data && cJSON_IsString(inline_data)) {
        meta->inline_data = buckets_strdup(inline_data->valuestring);
    }

    cJSON_Delete(root);
    return 0;
}

/* Free xl.meta resources */
void buckets_xl_meta_free(buckets_xl_meta_t *meta)
{
    if (!meta) {
        return;
    }

    /* Free erasure data */
    if (meta->erasure.distribution) {
        buckets_free(meta->erasure.distribution);
        meta->erasure.distribution = NULL;
    }

    if (meta->erasure.checksums) {
        buckets_free(meta->erasure.checksums);
        meta->erasure.checksums = NULL;
    }

    /* Free meta data */
    if (meta->meta.content_type) {
        buckets_free(meta->meta.content_type);
        meta->meta.content_type = NULL;
    }

    if (meta->meta.etag) {
        buckets_free(meta->meta.etag);
        meta->meta.etag = NULL;
    }

    /* Free user metadata */
    for (u32 i = 0; i < meta->meta.user_count; i++) {
        if (meta->meta.user_keys[i]) {
            buckets_free(meta->meta.user_keys[i]);
        }
        if (meta->meta.user_values[i]) {
            buckets_free(meta->meta.user_values[i]);
        }
    }

    if (meta->meta.user_keys) {
        buckets_free(meta->meta.user_keys);
        meta->meta.user_keys = NULL;
    }

    if (meta->meta.user_values) {
        buckets_free(meta->meta.user_values);
        meta->meta.user_values = NULL;
    }

    /* Free inline data */
    if (meta->inline_data) {
        buckets_free(meta->inline_data);
        meta->inline_data = NULL;
    }

    meta->meta.user_count = 0;
}

/* Read xl.meta from disk */
int buckets_read_xl_meta(const char *disk_path, const char *object_path,
                         buckets_xl_meta_t *meta)
{
    if (!disk_path || !object_path || !meta) {
        buckets_error("NULL parameter in read_xl_meta");
        return -1;
    }

    /* Construct xl.meta path */
    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/%sxl.meta", 
             disk_path, object_path);

    /* Read file */
    char *json = NULL;
    size_t json_size = 0;
    if (buckets_atomic_read(meta_path, (void**)&json, &json_size) != 0) {
        buckets_error("Failed to read xl.meta: %s", meta_path);
        return -1;
    }

    /* Parse JSON */
    int result = buckets_xl_meta_from_json(json, meta);
    buckets_free(json);

    return result;
}

/* Write xl.meta to disk (atomic) */
int buckets_write_xl_meta(const char *disk_path, const char *object_path,
                          const buckets_xl_meta_t *meta)
{
    if (!disk_path || !object_path || !meta) {
        buckets_error("NULL parameter in write_xl_meta");
        return -1;
    }

    /* Serialize to JSON */
    char *json = buckets_xl_meta_to_json(meta);
    if (!json) {
        buckets_error("Failed to serialize xl.meta");
        return -1;
    }

    /* Construct xl.meta path */
    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/%sxl.meta", 
             disk_path, object_path);

    /* Write atomically */
    int result = buckets_atomic_write(meta_path, json, strlen(json));
    buckets_free(json);

    if (result != 0) {
        buckets_error("Failed to write xl.meta: %s", meta_path);
        return -1;
    }

    return 0;
}
