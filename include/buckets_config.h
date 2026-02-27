/**
 * Configuration Management
 * 
 * JSON configuration file parsing and validation
 */

#ifndef BUCKETS_CONFIG_H
#define BUCKETS_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "buckets.h"

/**
 * Node configuration
 */
typedef struct {
    char *id;               /* Node ID (e.g., "node1") */
    char *address;          /* Node address (e.g., "localhost") */
    int port;               /* Node port (e.g., 9001) */
    char *endpoint;         /* Node endpoint (e.g., "http://localhost:9001") */
    char *data_dir;         /* Data directory (e.g., "/tmp/buckets-node1") */
} buckets_node_config_t;

/**
 * Disk configuration (for node config file)
 */
typedef struct {
    char **disks;           /* Array of disk paths */
    int disk_count;         /* Number of disks */
} buckets_disk_config_t;

/**
 * Cluster node definition (from cluster.nodes array)
 */
typedef struct {
    char *id;               /* Node ID (e.g., "node1") */
    char *endpoint;         /* Node endpoint (e.g., "http://localhost:9001") */
    char **disks;           /* Array of disk paths for this node */
    int disk_count;         /* Number of disks */
} buckets_cluster_node_t;

/**
 * Cluster configuration
 */
typedef struct {
    bool enabled;           /* Enable clustering */
    char **peers;           /* Array of peer addresses (deprecated, use nodes) */
    int peer_count;         /* Number of peers */
    buckets_cluster_node_t *nodes;  /* Array of cluster nodes with disk info */
    int node_count;         /* Number of nodes */
    int sets;               /* Number of erasure sets */
    int disks_per_set;      /* Disks per erasure set */
} buckets_cluster_config_t;

/**
 * Erasure coding configuration
 */
typedef struct {
    bool enabled;           /* Enable erasure coding */
    int data_shards;        /* K (data shards) */
    int parity_shards;      /* M (parity shards) */
} buckets_erasure_config_t;

/**
 * Server configuration
 */
typedef struct {
    char *bind_address;     /* Bind address (e.g., "0.0.0.0") */
    int bind_port;          /* Bind port (e.g., 9000) */
} buckets_server_config_t;

/**
 * Complete configuration
 */
typedef struct {
    buckets_node_config_t node;
    buckets_disk_config_t storage;
    buckets_cluster_config_t cluster;
    buckets_erasure_config_t erasure;
    buckets_server_config_t server;
} buckets_config_t;

/**
 * Load configuration from JSON file
 * 
 * @param filepath Path to JSON configuration file
 * @return Configuration structure, or NULL on error
 */
buckets_config_t* buckets_config_load(const char *filepath);

/**
 * Free configuration structure
 * 
 * @param config Configuration to free
 */
void buckets_config_free(buckets_config_t *config);

/**
 * Validate configuration
 * 
 * @param config Configuration to validate
 * @return BUCKETS_OK on success, error code otherwise
 */
buckets_error_t buckets_config_validate(const buckets_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_CONFIG_H */
