/**
 * Topology Management Implementation
 * 
 * Handles topology.json creation, serialization, and management.
 * Topology defines the dynamic cluster state including:
 * - Set states (active/draining/removed)
 * - Generation number (version tracking)
 * - Virtual node factor for consistent hashing
 * - Disk endpoints and capacities
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "buckets.h"
#include "buckets_cluster.h"
#include "buckets_io.h"
#include "buckets_json.h"

/* Topology version constant */
#define BUCKETS_TOPOLOGY_VERSION 1
#define BUCKETS_VNODE_FACTOR 150  /* Virtual nodes per set */

/* Forward declarations */
static cJSON* topology_to_json(buckets_cluster_topology_t *topology);
static buckets_cluster_topology_t* topology_from_json(cJSON *json);

buckets_cluster_topology_t* buckets_topology_new(void)
{
    buckets_cluster_topology_t *topology = buckets_calloc(1, sizeof(buckets_cluster_topology_t));
    
    topology->version = BUCKETS_TOPOLOGY_VERSION;
    topology->generation = 0;  /* Start at 0 (unconfigured) */
    topology->vnode_factor = BUCKETS_VNODE_FACTOR;
    topology->pool_count = 0;
    topology->pools = NULL;
    
    /* Deployment ID will be set when converting from format or loading */
    topology->deployment_id[0] = '\0';
    
    buckets_debug("Created new topology: version=%d, generation=%ld, vnode_factor=%d",
                  topology->version, topology->generation, topology->vnode_factor);
    
    return topology;
}

void buckets_topology_free(buckets_cluster_topology_t *topology)
{
    if (!topology) {
        return;
    }
    
    /* Free pools */
    if (topology->pools) {
        for (int i = 0; i < topology->pool_count; i++) {
            buckets_pool_topology_t *pool = &topology->pools[i];
            if (pool->sets) {
                for (int j = 0; j < pool->set_count; j++) {
                    buckets_set_topology_t *set = &pool->sets[j];
                    buckets_free(set->disks);
                }
                buckets_free(pool->sets);
            }
        }
        buckets_free(topology->pools);
    }
    
    buckets_free(topology);
}

buckets_cluster_topology_t* buckets_topology_from_format(buckets_format_t *format)
{
    if (!format) {
        buckets_error("Cannot create topology from NULL format");
        return NULL;
    }
    
    buckets_cluster_topology_t *topology = buckets_topology_new();
    
    /* Set generation to 1 (first configuration) */
    topology->generation = 1;
    
    /* Copy deployment ID from format */
    memcpy(topology->deployment_id, format->meta.deployment_id, 
           sizeof(topology->deployment_id));
    
    /* Create single pool from format */
    topology->pool_count = 1;
    topology->pools = buckets_calloc(1, sizeof(buckets_pool_topology_t));
    
    buckets_pool_topology_t *pool = &topology->pools[0];
    pool->idx = 0;
    pool->set_count = format->erasure.set_count;
    pool->sets = buckets_calloc(pool->set_count, sizeof(buckets_set_topology_t));
    
    /* Create sets from format */
    for (int i = 0; i < format->erasure.set_count; i++) {
        buckets_set_topology_t *set = &pool->sets[i];
        set->idx = i;
        set->state = SET_STATE_ACTIVE;  /* All sets start as active */
        set->disk_count = format->erasure.disks_per_set;
        set->disks = buckets_calloc(set->disk_count, sizeof(buckets_disk_info_t));
        
        /* Create disk info from format UUIDs */
        for (int j = 0; j < format->erasure.disks_per_set; j++) {
            buckets_disk_info_t *disk = &set->disks[j];
            
            /* Copy UUID */
            strncpy(disk->uuid, format->erasure.sets[i][j], sizeof(disk->uuid) - 1);
            
            /* Endpoint will be set later when resolving endpoints */
            disk->endpoint[0] = '\0';
            
            /* Capacity will be detected later */
            disk->capacity = 0;
        }
    }
    
    buckets_info("Created topology from format: deployment_id=%s, sets=%d, disks_per_set=%d",
                 topology->deployment_id, format->erasure.set_count, 
                 format->erasure.disks_per_set);
    
    return topology;
}

static const char* set_state_to_string(buckets_set_state_t state)
{
    switch (state) {
        case SET_STATE_ACTIVE: return "active";
        case SET_STATE_DRAINING: return "draining";
        case SET_STATE_REMOVED: return "removed";
        default: return "unknown";
    }
}

static buckets_set_state_t set_state_from_string(const char *str)
{
    if (!str) return SET_STATE_ACTIVE;
    
    if (strcmp(str, "active") == 0) return SET_STATE_ACTIVE;
    if (strcmp(str, "draining") == 0) return SET_STATE_DRAINING;
    if (strcmp(str, "removed") == 0) return SET_STATE_REMOVED;
    
    return SET_STATE_ACTIVE;  /* Default */
}

static cJSON* topology_to_json(buckets_cluster_topology_t *topology)
{
    if (!topology) {
        return NULL;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        buckets_error("Failed to create JSON object");
        return NULL;
    }
    
    /* Add topology metadata */
    buckets_json_add_int(root, "version", topology->version);
    buckets_json_add_int(root, "generation", (int)topology->generation);
    buckets_json_add_string(root, "deploymentId", topology->deployment_id);
    buckets_json_add_int(root, "vnodeFactor", topology->vnode_factor);
    
    /* Create pools array */
    cJSON *pools_array = cJSON_CreateArray();
    for (int i = 0; i < topology->pool_count; i++) {
        buckets_pool_topology_t *pool = &topology->pools[i];
        cJSON *pool_obj = cJSON_CreateObject();
        
        buckets_json_add_int(pool_obj, "idx", pool->idx);
        
        /* Create sets array */
        cJSON *sets_array = cJSON_CreateArray();
        for (int j = 0; j < pool->set_count; j++) {
            buckets_set_topology_t *set = &pool->sets[j];
            cJSON *set_obj = cJSON_CreateObject();
            
            buckets_json_add_int(set_obj, "idx", set->idx);
            buckets_json_add_string(set_obj, "state", set_state_to_string(set->state));
            
            /* Create disks array */
            cJSON *disks_array = cJSON_CreateArray();
            for (int k = 0; k < set->disk_count; k++) {
                buckets_disk_info_t *disk = &set->disks[k];
                cJSON *disk_obj = cJSON_CreateObject();
                
                buckets_json_add_string(disk_obj, "uuid", disk->uuid);
                buckets_json_add_string(disk_obj, "endpoint", disk->endpoint);
                
                /* Use string for capacity to avoid JSON number precision issues */
                char capacity_str[32];
                snprintf(capacity_str, sizeof(capacity_str), "%lu", disk->capacity);
                buckets_json_add_string(disk_obj, "capacity", capacity_str);
                
                cJSON_AddItemToArray(disks_array, disk_obj);
            }
            buckets_json_add_array(set_obj, "disks", disks_array);
            
            cJSON_AddItemToArray(sets_array, set_obj);
        }
        buckets_json_add_array(pool_obj, "sets", sets_array);
        
        cJSON_AddItemToArray(pools_array, pool_obj);
    }
    buckets_json_add_array(root, "pools", pools_array);
    
    return root;
}

static buckets_cluster_topology_t* topology_from_json(cJSON *json)
{
    if (!json) {
        return NULL;
    }
    
    buckets_cluster_topology_t *topology = buckets_calloc(1, sizeof(buckets_cluster_topology_t));
    
    /* Parse topology metadata */
    topology->version = buckets_json_get_int(json, "version", BUCKETS_TOPOLOGY_VERSION);
    topology->generation = buckets_json_get_int(json, "generation", 1);
    topology->vnode_factor = buckets_json_get_int(json, "vnodeFactor", BUCKETS_VNODE_FACTOR);
    
    char *deployment_id = buckets_json_get_string(json, "deploymentId", NULL);
    if (!deployment_id) {
        buckets_error("Missing deployment ID in topology JSON");
        buckets_free(topology);
        return NULL;
    }
    strncpy(topology->deployment_id, deployment_id, sizeof(topology->deployment_id) - 1);
    buckets_free(deployment_id);
    
    /* Parse pools array */
    cJSON *pools_array = buckets_json_get_array(json, "pools");
    if (!pools_array) {
        buckets_error("Missing 'pools' array in topology JSON");
        buckets_free(topology);
        return NULL;
    }
    
    topology->pool_count = cJSON_GetArraySize(pools_array);
    if (topology->pool_count <= 0) {
        buckets_error("Invalid pool count: %d", topology->pool_count);
        buckets_free(topology);
        return NULL;
    }
    
    topology->pools = buckets_calloc(topology->pool_count, sizeof(buckets_pool_topology_t));
    
    /* Parse each pool */
    cJSON *pool_obj = NULL;
    int pool_idx = 0;
    cJSON_ArrayForEach(pool_obj, pools_array) {
        buckets_pool_topology_t *pool = &topology->pools[pool_idx];
        
        pool->idx = buckets_json_get_int(pool_obj, "idx", pool_idx);
        
        /* Parse sets array */
        cJSON *sets_array = buckets_json_get_array(pool_obj, "sets");
        if (!sets_array) {
            buckets_error("Missing 'sets' array in pool %d", pool_idx);
            buckets_topology_free(topology);
            return NULL;
        }
        
        pool->set_count = cJSON_GetArraySize(sets_array);
        pool->sets = buckets_calloc(pool->set_count, sizeof(buckets_set_topology_t));
        
        /* Parse each set */
        cJSON *set_obj = NULL;
        int set_idx = 0;
        cJSON_ArrayForEach(set_obj, sets_array) {
            buckets_set_topology_t *set = &pool->sets[set_idx];
            
            set->idx = buckets_json_get_int(set_obj, "idx", set_idx);
            
            char *state_str = buckets_json_get_string(set_obj, "state", "active");
            set->state = set_state_from_string(state_str);
            buckets_free(state_str);
            
            /* Parse disks array */
            cJSON *disks_array = buckets_json_get_array(set_obj, "disks");
            if (!disks_array) {
                buckets_error("Missing 'disks' array in set %d", set_idx);
                buckets_topology_free(topology);
                return NULL;
            }
            
            set->disk_count = cJSON_GetArraySize(disks_array);
            set->disks = buckets_calloc(set->disk_count, sizeof(buckets_disk_info_t));
            
            /* Parse each disk */
            cJSON *disk_obj = NULL;
            int disk_idx = 0;
            cJSON_ArrayForEach(disk_obj, disks_array) {
                buckets_disk_info_t *disk = &set->disks[disk_idx];
                
                char *uuid = buckets_json_get_string(disk_obj, "uuid", "");
                strncpy(disk->uuid, uuid, sizeof(disk->uuid) - 1);
                buckets_free(uuid);
                
                char *endpoint = buckets_json_get_string(disk_obj, "endpoint", "");
                strncpy(disk->endpoint, endpoint, sizeof(disk->endpoint) - 1);
                buckets_free(endpoint);
                
                char *capacity_str = buckets_json_get_string(disk_obj, "capacity", "0");
                disk->capacity = strtoull(capacity_str, NULL, 10);
                buckets_free(capacity_str);
                
                disk_idx++;
            }
            
            set_idx++;
        }
        
        pool_idx++;
    }
    
    buckets_debug("Parsed topology from JSON: deployment_id=%s, pools=%d, generation=%ld",
                  topology->deployment_id, topology->pool_count, topology->generation);
    
    return topology;
}

int buckets_topology_save(const char *disk_path, buckets_cluster_topology_t *topology)
{
    if (!disk_path || !topology) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Convert topology to JSON */
    cJSON *json = topology_to_json(topology);
    if (!json) {
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Get topology file path */
    char *topology_path = buckets_get_topology_path(disk_path);
    if (!topology_path) {
        cJSON_Delete(json);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Save JSON to disk atomically */
    int ret = buckets_json_save(topology_path, json, true);
    
    cJSON_Delete(json);
    buckets_free(topology_path);
    
    if (ret == BUCKETS_OK) {
        buckets_info("Saved topology to disk: %s (generation=%ld)",
                     disk_path, topology->generation);
    } else {
        buckets_error("Failed to save topology to disk: %s", disk_path);
    }
    
    return ret;
}

buckets_cluster_topology_t* buckets_topology_load(const char *disk_path)
{
    if (!disk_path) {
        return NULL;
    }
    
    /* Get topology file path */
    char *topology_path = buckets_get_topology_path(disk_path);
    if (!topology_path) {
        return NULL;
    }
    
    /* Load JSON from disk */
    cJSON *json = buckets_json_load(topology_path);
    buckets_free(topology_path);
    
    if (!json) {
        buckets_debug("Topology not found on disk: %s (will create from format.json)", disk_path);
        return NULL;
    }
    
    /* Parse JSON to topology structure */
    buckets_cluster_topology_t *topology = topology_from_json(json);
    cJSON_Delete(json);
    
    if (topology) {
        buckets_debug("Loaded topology from disk: %s (generation=%ld)",
                      disk_path, topology->generation);
    }
    
    return topology;
}
