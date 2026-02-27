/**
 * Configuration Management Implementation
 * 
 * JSON configuration file parsing using cJSON
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "buckets.h"
#include "buckets_config.h"
#include "cJSON.h"

/**
 * Helper: Read file contents into string
 */
static char* read_file(const char *filepath)
{
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        buckets_error("Failed to open config file: %s", filepath);
        return NULL;
    }
    
    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    /* Allocate buffer */
    char *buffer = buckets_malloc(size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }
    
    /* Read file */
    size_t read = fread(buffer, 1, size, fp);
    buffer[read] = '\0';
    fclose(fp);
    
    return buffer;
}

/**
 * Helper: Parse string array from JSON
 */
static int parse_string_array(cJSON *array, char ***out_strings, int *out_count)
{
    if (!cJSON_IsArray(array)) {
        return -1;
    }
    
    int count = cJSON_GetArraySize(array);
    char **strings = buckets_malloc(sizeof(char*) * count);
    if (!strings) {
        return -1;
    }
    
    int i = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item)) {
            /* Free already allocated strings */
            for (int j = 0; j < i; j++) {
                buckets_free(strings[j]);
            }
            buckets_free(strings);
            return -1;
        }
        strings[i] = buckets_strdup(item->valuestring);
        i++;
    }
    
    *out_strings = strings;
    *out_count = count;
    return 0;
}

/**
 * Load configuration from JSON file
 */
buckets_config_t* buckets_config_load(const char *filepath)
{
    if (!filepath) {
        buckets_error("Config filepath is NULL");
        return NULL;
    }
    
    buckets_info("Loading configuration from: %s", filepath);
    
    /* Read file */
    char *json_str = read_file(filepath);
    if (!json_str) {
        return NULL;
    }
    
    /* Parse JSON */
    cJSON *root = cJSON_Parse(json_str);
    buckets_free(json_str);
    
    if (!root) {
        buckets_error("Failed to parse JSON: %s", cJSON_GetErrorPtr());
        return NULL;
    }
    
    /* Allocate config */
    buckets_config_t *config = buckets_calloc(1, sizeof(buckets_config_t));
    if (!config) {
        cJSON_Delete(root);
        return NULL;
    }
    
    /* Parse node section */
    cJSON *node = cJSON_GetObjectItem(root, "node");
    if (node) {
        cJSON *id = cJSON_GetObjectItem(node, "id");
        if (id && cJSON_IsString(id)) {
            config->node.id = buckets_strdup(id->valuestring);
        }
        
        cJSON *address = cJSON_GetObjectItem(node, "address");
        if (address && cJSON_IsString(address)) {
            config->node.address = buckets_strdup(address->valuestring);
        }
        
        cJSON *port = cJSON_GetObjectItem(node, "port");
        if (port && cJSON_IsNumber(port)) {
            config->node.port = port->valueint;
        }
        
        cJSON *endpoint = cJSON_GetObjectItem(node, "endpoint");
        if (endpoint && cJSON_IsString(endpoint)) {
            config->node.endpoint = buckets_strdup(endpoint->valuestring);
        }
        
        cJSON *data_dir = cJSON_GetObjectItem(node, "data_dir");
        if (data_dir && cJSON_IsString(data_dir)) {
            config->node.data_dir = buckets_strdup(data_dir->valuestring);
        }
    }
    
    /* Parse storage section */
    cJSON *storage = cJSON_GetObjectItem(root, "storage");
    if (storage) {
        cJSON *disks = cJSON_GetObjectItem(storage, "disks");
        if (disks) {
            if (parse_string_array(disks, &config->storage.disks, 
                                   &config->storage.disk_count) != 0) {
                buckets_error("Failed to parse storage.disks array");
                cJSON_Delete(root);
                buckets_config_free(config);
                return NULL;
            }
        }
    }
    
    /* Parse cluster section */
    cJSON *cluster = cJSON_GetObjectItem(root, "cluster");
    if (cluster) {
        cJSON *enabled = cJSON_GetObjectItem(cluster, "enabled");
        if (enabled && cJSON_IsBool(enabled)) {
            config->cluster.enabled = cJSON_IsTrue(enabled);
        }
        
        cJSON *peers = cJSON_GetObjectItem(cluster, "peers");
        if (peers) {
            if (parse_string_array(peers, &config->cluster.peers,
                                   &config->cluster.peer_count) != 0) {
                buckets_error("Failed to parse cluster.peers array");
                cJSON_Delete(root);
                buckets_config_free(config);
                return NULL;
            }
        }
        
        /* Parse cluster.nodes array */
        cJSON *nodes = cJSON_GetObjectItem(cluster, "nodes");
        if (nodes && cJSON_IsArray(nodes)) {
            config->cluster.node_count = cJSON_GetArraySize(nodes);
            config->cluster.nodes = buckets_calloc(config->cluster.node_count, 
                                                   sizeof(buckets_cluster_node_t));
            
            int node_idx = 0;
            cJSON *node_item = NULL;
            cJSON_ArrayForEach(node_item, nodes) {
                buckets_cluster_node_t *node = &config->cluster.nodes[node_idx];
                
                /* Parse node id */
                cJSON *id = cJSON_GetObjectItem(node_item, "id");
                if (id && cJSON_IsString(id)) {
                    node->id = buckets_strdup(id->valuestring);
                }
                
                /* Parse node endpoint */
                cJSON *endpoint = cJSON_GetObjectItem(node_item, "endpoint");
                if (endpoint && cJSON_IsString(endpoint)) {
                    node->endpoint = buckets_strdup(endpoint->valuestring);
                }
                
                /* Parse node disks array */
                cJSON *disks = cJSON_GetObjectItem(node_item, "disks");
                if (disks && cJSON_IsArray(disks)) {
                    if (parse_string_array(disks, &node->disks, &node->disk_count) != 0) {
                        buckets_error("Failed to parse disks array for node %s", 
                                     node->id ? node->id : "unknown");
                        cJSON_Delete(root);
                        buckets_config_free(config);
                        return NULL;
                    }
                }
                
                node_idx++;
            }
        }
        
        cJSON *sets = cJSON_GetObjectItem(cluster, "sets");
        if (sets && cJSON_IsNumber(sets)) {
            config->cluster.sets = sets->valueint;
        }
        
        cJSON *disks_per_set = cJSON_GetObjectItem(cluster, "disks_per_set");
        if (disks_per_set && cJSON_IsNumber(disks_per_set)) {
            config->cluster.disks_per_set = disks_per_set->valueint;
        }
    }
    
    /* Parse erasure section */
    cJSON *erasure = cJSON_GetObjectItem(root, "erasure");
    if (erasure) {
        cJSON *enabled = cJSON_GetObjectItem(erasure, "enabled");
        if (enabled && cJSON_IsBool(enabled)) {
            config->erasure.enabled = cJSON_IsTrue(enabled);
        }
        
        cJSON *data_shards = cJSON_GetObjectItem(erasure, "data_shards");
        if (data_shards && cJSON_IsNumber(data_shards)) {
            config->erasure.data_shards = data_shards->valueint;
        }
        
        cJSON *parity_shards = cJSON_GetObjectItem(erasure, "parity_shards");
        if (parity_shards && cJSON_IsNumber(parity_shards)) {
            config->erasure.parity_shards = parity_shards->valueint;
        }
    }
    
    /* Parse server section */
    cJSON *server = cJSON_GetObjectItem(root, "server");
    if (server) {
        cJSON *bind_address = cJSON_GetObjectItem(server, "bind_address");
        if (bind_address && cJSON_IsString(bind_address)) {
            config->server.bind_address = buckets_strdup(bind_address->valuestring);
        }
        
        cJSON *bind_port = cJSON_GetObjectItem(server, "bind_port");
        if (bind_port && cJSON_IsNumber(bind_port)) {
            config->server.bind_port = bind_port->valueint;
        }
    }
    
    cJSON_Delete(root);
    
    buckets_info("Configuration loaded successfully");
    buckets_info("  Node: %s (%s:%d)", 
                 config->node.id ? config->node.id : "unknown",
                 config->node.address ? config->node.address : "unknown",
                 config->node.port);
    buckets_info("  Storage: %d disks", config->storage.disk_count);
    buckets_info("  Cluster: %s (%d peers, %d sets, %d disks/set)",
                 config->cluster.enabled ? "enabled" : "disabled",
                 config->cluster.peer_count,
                 config->cluster.sets,
                 config->cluster.disks_per_set);
    buckets_info("  Erasure: %s (K=%d, M=%d)",
                 config->erasure.enabled ? "enabled" : "disabled",
                 config->erasure.data_shards,
                 config->erasure.parity_shards);
    buckets_info("  Server: %s:%d",
                 config->server.bind_address ? config->server.bind_address : "unknown",
                 config->server.bind_port);
    
    return config;
}

/**
 * Free configuration structure
 */
void buckets_config_free(buckets_config_t *config)
{
    if (!config) {
        return;
    }
    
    /* Free node section */
    buckets_free(config->node.id);
    buckets_free(config->node.address);
    buckets_free(config->node.endpoint);
    buckets_free(config->node.data_dir);
    
    /* Free storage section */
    for (int i = 0; i < config->storage.disk_count; i++) {
        buckets_free(config->storage.disks[i]);
    }
    buckets_free(config->storage.disks);
    
    /* Free cluster */
    if (config->cluster.peers) {
        for (int i = 0; i < config->cluster.peer_count; i++) {
            buckets_free(config->cluster.peers[i]);
        }
        buckets_free(config->cluster.peers);
    }
    
    if (config->cluster.nodes) {
        for (int i = 0; i < config->cluster.node_count; i++) {
            buckets_cluster_node_t *node = &config->cluster.nodes[i];
            if (node->id) buckets_free(node->id);
            if (node->endpoint) buckets_free(node->endpoint);
            if (node->disks) {
                for (int j = 0; j < node->disk_count; j++) {
                    buckets_free(node->disks[j]);
                }
                buckets_free(node->disks);
            }
        }
        buckets_free(config->cluster.nodes);
    }
    buckets_free(config->cluster.peers);
    
    /* Free server section */
    buckets_free(config->server.bind_address);
    
    /* Free config itself */
    buckets_free(config);
}

/**
 * Validate configuration
 */
buckets_error_t buckets_config_validate(const buckets_config_t *config)
{
    if (!config) {
        buckets_error("Config is NULL");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Validate node section */
    if (!config->node.id || strlen(config->node.id) == 0) {
        buckets_error("node.id is required");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (!config->node.address || strlen(config->node.address) == 0) {
        buckets_error("node.address is required");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (config->node.port <= 0 || config->node.port > 65535) {
        buckets_error("node.port must be between 1 and 65535");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (!config->node.data_dir || strlen(config->node.data_dir) == 0) {
        buckets_error("node.data_dir is required");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Validate storage section */
    if (config->storage.disk_count <= 0) {
        buckets_error("storage.disks must have at least one disk");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Validate server section */
    if (!config->server.bind_address || strlen(config->server.bind_address) == 0) {
        buckets_error("server.bind_address is required");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    if (config->server.bind_port <= 0 || config->server.bind_port > 65535) {
        buckets_error("server.bind_port must be between 1 and 65535");
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Validate erasure coding if enabled */
    if (config->erasure.enabled) {
        if (config->erasure.data_shards <= 0) {
            buckets_error("erasure.data_shards must be positive");
            return BUCKETS_ERR_INVALID_ARG;
        }
        
        if (config->erasure.parity_shards <= 0) {
            buckets_error("erasure.parity_shards must be positive");
            return BUCKETS_ERR_INVALID_ARG;
        }
        
        int total_shards = config->erasure.data_shards + config->erasure.parity_shards;
        
        /* In cluster mode, use cluster.disks_per_set instead of storage.disk_count */
        int available_disks = config->cluster.enabled ? config->cluster.disks_per_set : config->storage.disk_count;
        
        if (available_disks < total_shards) {
            buckets_error("%s count (%d) must be >= K+M (%d)",
                         config->cluster.enabled ? "cluster.disks_per_set" : "storage.disks",
                         available_disks, total_shards);
            return BUCKETS_ERR_INVALID_ARG;
        }
    }
    
    buckets_info("Configuration validation passed");
    return BUCKETS_OK;
}
