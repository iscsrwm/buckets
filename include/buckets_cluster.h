/**
 * Buckets Cluster and State Management
 * 
 * Defines cluster topology, format metadata, and state management
 */

#ifndef BUCKETS_CLUSTER_H
#define BUCKETS_CLUSTER_H

#include <pthread.h>
#include "buckets.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Format Metadata (format.json) ===== */

typedef struct {
    char version[16];           // Format version ("1")
    char format_type[16];       // Backend type ("erasure")
    char deployment_id[37];     // Cluster UUID (36 chars + null)
} buckets_format_meta_t;

typedef struct {
    char version[16];           // Erasure format version ("3")
    char this_disk[37];         // This disk's UUID
    
    // Set topology: [set_count][disks_per_set] = disk_uuid
    char ***sets;
    int set_count;
    int disks_per_set;
    
    char distribution_algo[32]; // "SIPMOD+PARITY"
} buckets_erasure_info_t;

typedef struct {
    buckets_format_meta_t meta;
    buckets_erasure_info_t erasure;
} buckets_format_t;

/* Format operations */
buckets_format_t* buckets_format_new(int set_count, int disks_per_set);
void buckets_format_free(buckets_format_t *format);
buckets_format_t* buckets_format_load(const char *disk_path);
int buckets_format_save(const char *disk_path, buckets_format_t *format);
int buckets_format_validate(buckets_format_t **formats, int count);
buckets_format_t* buckets_format_clone(buckets_format_t *format);

/* ===== Topology (topology.json) ===== */

#define BUCKETS_VNODE_FACTOR 150  /* Virtual nodes per set for consistent hashing */

typedef enum {
    SET_STATE_ACTIVE = 0,
    SET_STATE_DRAINING,
    SET_STATE_REMOVED
} buckets_set_state_t;

typedef struct {
    char endpoint[256];         // "http://node1:9000/mnt/disk1"
    char uuid[37];              // Disk UUID
    u64 capacity;               // Bytes
} buckets_disk_info_t;

typedef struct {
    int idx;                        // Set index
    buckets_set_state_t state;      // Active, draining, removed
    buckets_disk_info_t *disks;     // Array of disks
    int disk_count;
} buckets_set_topology_t;

typedef struct {
    int idx;                            // Pool index
    buckets_set_topology_t *sets;       // Array of sets
    int set_count;
} buckets_pool_topology_t;

typedef struct {
    int version;                        // Topology version (1)
    i64 generation;                     // Increments on changes
    char deployment_id[37];             // Cluster UUID
    int vnode_factor;                   // Virtual nodes per set (150)
    
    buckets_pool_topology_t *pools;     // Array of pools
    int pool_count;
} buckets_cluster_topology_t;

/* Topology operations */
buckets_cluster_topology_t* buckets_topology_new(void);
void buckets_topology_free(buckets_cluster_topology_t *topology);
buckets_cluster_topology_t* buckets_topology_load(const char *disk_path);
int buckets_topology_save(const char *disk_path, buckets_cluster_topology_t *topology);
buckets_cluster_topology_t* buckets_topology_from_format(buckets_format_t *format);

/* ===== Endpoints ===== */

typedef struct {
    char url[256];              // "http://node1:9000/mnt/disk1"
    bool is_local;              // Is this endpoint on current node?
    int pool_idx;               // Pool index
    int set_idx;                // Set index within pool
    int disk_idx;               // Disk index within set
} buckets_endpoint_t;

typedef struct {
    buckets_endpoint_t *endpoints;
    int endpoint_count;
    int set_count;
    int drives_per_set;
    char cmdline[1024];
} buckets_pool_endpoints_t;

typedef struct {
    buckets_pool_endpoints_t *pools;
    int pool_count;
} buckets_endpoint_pools_t;

/* Endpoint operations */
buckets_endpoint_pools_t* buckets_endpoints_parse(const char *cmdline);
void buckets_endpoints_free(buckets_endpoint_pools_t *endpoints);
int buckets_endpoints_resolve(buckets_endpoint_pools_t *endpoints);

/* ===== Server Pools ===== */

typedef struct {
    buckets_set_topology_t *sets;       // Erasure sets
    buckets_endpoint_t *endpoints;      // Pool endpoints
    int set_count;
    int set_drive_count;
    char distribution_algo[32];
    int default_parity_count;
} buckets_erasure_sets_t;

typedef struct {
    pthread_rwlock_t pool_meta_mutex;
    
    u8 deployment_id[16];                   // Binary deployment ID
    char distribution_algo[32];
    
    buckets_erasure_sets_t **server_pools;  // Array of pools
    int pool_count;
    
    // Location registry and consistent hash ring (opaque pointers)
    void *location_registry;
    void *consistent_hash_ring;
} buckets_server_pools_t;

/* Server pool operations */
buckets_server_pools_t* buckets_server_pools_new(buckets_endpoint_pools_t *endpoints);
void buckets_server_pools_free(buckets_server_pools_t *pools);
int buckets_server_pools_init(buckets_server_pools_t *pools);

/* ===== Global State ===== */

extern buckets_endpoint_pools_t *g_endpoints;
extern u8 g_deployment_id[16];
extern char g_local_node_name[256];
extern buckets_server_pools_t *g_server_pools;

/* ===== Utilities ===== */

/* UUID generation and parsing */
void buckets_uuid_generate(char *uuid_str);        // Generate UUID string (37 bytes)
int buckets_uuid_parse(const char *str, u8 *uuid); // Parse UUID string to bytes
void buckets_uuid_to_string(const u8 *uuid, char *str);

/* Atomic file operations */
int buckets_atomic_write(const char *path, const void *data, size_t len);
int buckets_atomic_read(const char *path, void **data, size_t *len);

/* Quorum operations */
int buckets_quorum_write(const char **disk_paths, int disk_count,
                         const char *rel_path, const void *data, size_t len);
buckets_result_t buckets_quorum_read(const char **disk_paths, int disk_count,
                                     const char *rel_path);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_CLUSTER_H */
