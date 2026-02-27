/**
 * Object Placement Implementation - Consistent Hashing
 * 
 * Implements consistent hashing with virtual nodes for deterministic 
 * object placement with minimal data movement during topology changes.
 * 
 * Reference: architecture/SCALE_AND_DATA_PLACEMENT.md Section 6.2
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buckets.h"
#include "buckets_placement.h"
#include "buckets_cluster.h"
#include "buckets_hash.h"

/* Configuration */
#define VNODES_PER_SET 150  /* Virtual nodes per erasure set */

/* Global state */
static bool g_placement_initialized = false;
static u64 g_siphash_k0 = 0;  /* SipHash key - from deployment ID */
static u64 g_siphash_k1 = 0;

/* Hash ring state */
static buckets_placement_vnode_t *g_hash_ring = NULL;
static size_t g_hash_ring_size = 0;
static u64 g_current_generation = 0;

/**
 * Compare function for sorting vnodes by hash
 */
static int vnode_compare(const void *a, const void *b)
{
    const buckets_placement_vnode_t *va = (const buckets_placement_vnode_t *)a;
    const buckets_placement_vnode_t *vb = (const buckets_placement_vnode_t *)b;
    
    if (va->hash < vb->hash) return -1;
    if (va->hash > vb->hash) return 1;
    return 0;
}

/**
 * Build hash ring from topology
 * 
 * Creates VNODES_PER_SET virtual nodes for each active erasure set
 * and sorts them by hash value for efficient binary search.
 */
static int build_hash_ring(void)
{
    buckets_cluster_topology_t *topology = buckets_topology_manager_get();
    if (!topology) {
        buckets_error("Topology not available");
        return -1;
    }
    
    /* Count active sets */
    size_t total_active_sets = 0;
    for (int p = 0; p < topology->pool_count; p++) {
        buckets_pool_topology_t *pool = &topology->pools[p];
        for (int s = 0; s < pool->set_count; s++) {
            if (pool->sets[s].state == SET_STATE_ACTIVE) {
                total_active_sets++;
            }
        }
    }
    
    if (total_active_sets == 0) {
        buckets_error("No active sets in topology");
        return -1;
    }
    
    /* Allocate hash ring */
    size_t vnode_count = total_active_sets * VNODES_PER_SET;
    buckets_placement_vnode_t *ring = buckets_malloc(vnode_count * sizeof(buckets_placement_vnode_t));
    if (!ring) {
        return -1;
    }
    
    buckets_debug("Building hash ring: %zu active sets, %zu vnodes",
                  total_active_sets, vnode_count);
    
    /* Create virtual nodes for each active set */
    size_t vnode_idx = 0;
    for (int p = 0; p < topology->pool_count; p++) {
        buckets_pool_topology_t *pool = &topology->pools[p];
        for (int s = 0; s < pool->set_count; s++) {
            if (pool->sets[s].state != SET_STATE_ACTIVE) {
                continue;  /* Skip non-active sets */
            }
            
            /* Create VNODES_PER_SET virtual nodes for this set */
            for (int v = 0; v < VNODES_PER_SET; v++) {
                /* Generate unique vnode key: "pool:set:vnode_index" */
                char vnode_key[256];
                snprintf(vnode_key, sizeof(vnode_key), "%d:%d:%d", p, s, v);
                
                /* Hash the vnode key to get position on ring */
                u64 vnode_hash = buckets_siphash(g_siphash_k0, g_siphash_k1,
                                                 vnode_key, strlen(vnode_key));
                
                ring[vnode_idx].hash = vnode_hash;
                ring[vnode_idx].pool_idx = p;
                ring[vnode_idx].set_idx = s;
                vnode_idx++;
            }
        }
    }
    
    /* Sort vnodes by hash for binary search */
    qsort(ring, vnode_count, sizeof(buckets_placement_vnode_t), vnode_compare);
    
    /* Replace old ring */
    if (g_hash_ring) {
        buckets_free(g_hash_ring);
    }
    g_hash_ring = ring;
    g_hash_ring_size = vnode_count;
    g_current_generation = topology->generation;
    
    buckets_info("Hash ring built: %zu vnodes, generation=%llu",
                 vnode_count, (unsigned long long)g_current_generation);
    
    return 0;
}

/**
 * Binary search for next vnode >= target hash
 * 
 * Returns index of first vnode with hash >= target_hash,
 * or wraps around to index 0 if target_hash > all vnodes.
 */
static size_t find_vnode(u64 target_hash)
{
    if (g_hash_ring_size == 0) {
        return 0;
    }
    
    /* Binary search for first vnode with hash >= target_hash */
    size_t left = 0;
    size_t right = g_hash_ring_size;
    
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        
        if (g_hash_ring[mid].hash < target_hash) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    
    /* Wrap around if we're past the end of the ring */
    if (left >= g_hash_ring_size) {
        return 0;
    }
    
    return left;
}

/**
 * Initialize placement system
 */
int buckets_placement_init(void)
{
    if (g_placement_initialized) {
        buckets_warn("Placement already initialized");
        return 0;
    }
    
    /* Get topology to derive SipHash keys from deployment ID */
    buckets_cluster_topology_t *topology = buckets_topology_manager_get();
    if (!topology) {
        buckets_error("Topology not available for placement initialization");
        return -1;
    }
    
    /* Parse deployment ID to get hash keys */
    u8 deployment_uuid[16];
    if (buckets_uuid_parse(topology->deployment_id, deployment_uuid) != 0) {
        buckets_error("Failed to parse deployment ID");
        return -1;
    }
    
    /* Use first 16 bytes of deployment UUID as SipHash keys */
    memcpy(&g_siphash_k0, deployment_uuid, 8);
    memcpy(&g_siphash_k1, deployment_uuid + 8, 8);
    
    buckets_info("Placement initialized: k0=%016llx, k1=%016llx",
                 (unsigned long long)g_siphash_k0,
                 (unsigned long long)g_siphash_k1);
    
    /* Build initial hash ring */
    if (build_hash_ring() != 0) {
        buckets_error("Failed to build hash ring");
        return -1;
    }
    
    g_placement_initialized = true;
    
    return 0;
}

/**
 * Rebuild hash ring from current topology
 */
int buckets_placement_rebuild_ring(void)
{
    if (!g_placement_initialized) {
        buckets_error("Placement not initialized");
        return -1;
    }
    
    buckets_info("Rebuilding hash ring...");
    return build_hash_ring();
}

/**
 * Cleanup placement system
 */
void buckets_placement_cleanup(void)
{
    if (g_hash_ring) {
        buckets_free(g_hash_ring);
        g_hash_ring = NULL;
    }
    g_hash_ring_size = 0;
    g_placement_initialized = false;
    g_siphash_k0 = 0;
    g_siphash_k1 = 0;
}

/**
 * Compute consistent hash placement for an object
 */
int buckets_placement_compute(const char *bucket, const char *object,
                              buckets_placement_result_t **result)
{
    if (!bucket || !object || !result) {
        buckets_error("NULL parameter in placement_compute");
        return -1;
    }
    
    if (!g_placement_initialized) {
        buckets_error("Placement not initialized");
        return -1;
    }
    
    if (g_hash_ring_size == 0) {
        buckets_error("Hash ring is empty");
        return -1;
    }
    
    /* Get current topology */
    buckets_cluster_topology_t *topology = buckets_topology_manager_get();
    if (!topology) {
        buckets_error("Topology not available");
        return -1;
    }
    
    /* Compute object path: "bucket/object" */
    size_t path_len = strlen(bucket) + 1 + strlen(object) + 1;
    char *object_path = buckets_malloc(path_len);
    if (!object_path) {
        return -1;
    }
    snprintf(object_path, path_len, "%s/%s", bucket, object);
    
    /* Hash object path using SipHash */
    u64 object_hash = buckets_siphash(g_siphash_k0, g_siphash_k1,
                                      object_path, strlen(object_path));
    buckets_free(object_path);
    
    /* Find next vnode on ring using binary search */
    size_t vnode_idx = find_vnode(object_hash);
    buckets_placement_vnode_t *vnode = &g_hash_ring[vnode_idx];
    
    u32 pool_idx = vnode->pool_idx;
    u32 set_idx = vnode->set_idx;
    
    buckets_debug("Consistent hash placement: hash=%016llx, vnode_idx=%zu/%zu, pool=%u, set=%u",
                  (unsigned long long)object_hash, vnode_idx, g_hash_ring_size,
                  pool_idx, set_idx);
    
    /* Get set from topology */
    if (pool_idx >= (u32)topology->pool_count) {
        buckets_error("Invalid pool index: %u >= %d", pool_idx, topology->pool_count);
        return -1;
    }
    
    buckets_pool_topology_t *pool = &topology->pools[pool_idx];
    if (set_idx >= (u32)pool->set_count) {
        buckets_error("Invalid set index: %u >= %d", set_idx, pool->set_count);
        return -1;
    }
    
    buckets_set_topology_t *set = &pool->sets[set_idx];
    
    /* Allocate result */
    buckets_placement_result_t *placement = buckets_calloc(1, sizeof(buckets_placement_result_t));
    if (!placement) {
        return -1;
    }
    
    placement->pool_idx = pool_idx;
    placement->set_idx = set_idx;
    placement->disk_count = set->disk_count;
    placement->generation = topology->generation;
    placement->state = set->state;
    placement->object_hash = object_hash;
    placement->vnode_index = vnode_idx;
    
    /* Allocate disk arrays */
    placement->disk_paths = buckets_calloc(set->disk_count, sizeof(char*));
    placement->disk_uuids = buckets_calloc(set->disk_count, sizeof(char*));
    placement->disk_endpoints = buckets_calloc(set->disk_count, sizeof(char*));
    
    if (!placement->disk_paths || !placement->disk_uuids || !placement->disk_endpoints) {
        buckets_placement_free_result(placement);
        return -1;
    }
    
    /* Copy disk information */
    /* Check if we need to get disk paths from multidisk layer */
    bool use_multidisk_paths = false;
    if (set->disk_count > 0 && set->disks[0].endpoint[0] == '\0') {
        /* Endpoints are empty - fall back to multidisk layer */
        use_multidisk_paths = true;
        buckets_info("Topology endpoints empty, using multidisk paths for set %u", set_idx);
    } else if (set->disk_count > 0) {
        buckets_info("Using topology endpoints for set %u (first endpoint: %.50s...)", 
                     set_idx, set->disks[0].endpoint);
    }
    
    if (use_multidisk_paths) {
        /* Get disk paths from multidisk layer */
        extern int buckets_multidisk_get_set_disks(int set_index, char **disk_paths, int max_disks);
        char *multidisk_paths[64];
        int multidisk_count = buckets_multidisk_get_set_disks(set_idx, multidisk_paths, 64);
        
        if (multidisk_count >= set->disk_count) {
            for (int i = 0; i < set->disk_count; i++) {
                placement->disk_paths[i] = buckets_strdup(multidisk_paths[i]);
                placement->disk_uuids[i] = buckets_strdup(set->disks[i].uuid);
                placement->disk_endpoints[i] = buckets_strdup("");  /* Empty for local-only mode */
                
                if (!placement->disk_paths[i] || !placement->disk_uuids[i] || !placement->disk_endpoints[i]) {
                    buckets_placement_free_result(placement);
                    return -1;
                }
                buckets_debug("Disk %d: path=%s, uuid=%s", i, placement->disk_paths[i], 
                             placement->disk_uuids[i]);
            }
        } else {
            buckets_error("Multidisk layer returned insufficient disks: %d < %d",
                         multidisk_count, set->disk_count);
            buckets_placement_free_result(placement);
            return -1;
        }
    } else {
        /* Use endpoints from topology */
        for (int i = 0; i < set->disk_count; i++) {
            /* Store full endpoint */
            const char *endpoint = set->disks[i].endpoint;
            placement->disk_endpoints[i] = buckets_strdup(endpoint);
            
            /* Extract path from endpoint (e.g., "http://node1:9000/mnt/disk1" -> "/mnt/disk1") */
            const char *path_start = strrchr(endpoint, '/');
            
            if (path_start) {
                /* Find the path portion (everything after the domain:port) */
                /* Format: http://domain:port/path */
                const char *scheme_end = strstr(endpoint, "://");
                if (scheme_end) {
                    scheme_end += 3;  /* Skip "://" */
                    const char *port_end = strchr(scheme_end, '/');
                    if (port_end) {
                        path_start = port_end;
                    }
                }
            }
            
            if (!path_start || path_start[0] != '/') {
                /* Fallback: use entire endpoint */
                placement->disk_paths[i] = buckets_strdup(endpoint);
            } else {
                placement->disk_paths[i] = buckets_strdup(path_start);
            }
            
            placement->disk_uuids[i] = buckets_strdup(set->disks[i].uuid);
            
            if (!placement->disk_paths[i] || !placement->disk_uuids[i] || !placement->disk_endpoints[i]) {
                buckets_placement_free_result(placement);
                return -1;
            }
        }
    }
    
    *result = placement;
    
    buckets_debug("Placement computed: pool=%u, set=%u, disks=%u, vnode=%zu/%zu",
                  pool_idx, set_idx, set->disk_count, vnode_idx, g_hash_ring_size);
    
    return 0;
}

/**
 * Free placement result
 */
void buckets_placement_free_result(buckets_placement_result_t *result)
{
    if (!result) {
        return;
    }
    
    if (result->disk_paths) {
        for (u32 i = 0; i < result->disk_count; i++) {
            buckets_free(result->disk_paths[i]);
        }
        buckets_free(result->disk_paths);
    }
    
    if (result->disk_uuids) {
        for (u32 i = 0; i < result->disk_count; i++) {
            buckets_free(result->disk_uuids[i]);
        }
        buckets_free(result->disk_uuids);
    }
    
    if (result->disk_endpoints) {
        for (u32 i = 0; i < result->disk_count; i++) {
            buckets_free(result->disk_endpoints[i]);
        }
        buckets_free(result->disk_endpoints);
    }
    
    buckets_free(result);
}

/**
 * Get placement statistics
 */
void buckets_placement_get_stats(u32 *total_sets, u32 *total_disks, double *avg_disks_per_set)
{
    if (!g_placement_initialized) {
        if (total_sets) *total_sets = 0;
        if (total_disks) *total_disks = 0;
        if (avg_disks_per_set) *avg_disks_per_set = 0.0;
        return;
    }
    
    buckets_cluster_topology_t *topology = buckets_topology_manager_get();
    if (!topology || topology->pool_count == 0) {
        if (total_sets) *total_sets = 0;
        if (total_disks) *total_disks = 0;
        if (avg_disks_per_set) *avg_disks_per_set = 0.0;
        return;
    }
    
    u32 set_count = 0;
    u32 disk_count = 0;
    
    for (int p = 0; p < topology->pool_count; p++) {
        buckets_pool_topology_t *pool = &topology->pools[p];
        for (int s = 0; s < pool->set_count; s++) {
            if (pool->sets[s].state == SET_STATE_ACTIVE) {
                set_count++;
                disk_count += pool->sets[s].disk_count;
            }
        }
    }
    
    if (total_sets) *total_sets = set_count;
    if (total_disks) *total_disks = disk_count;
    if (avg_disks_per_set) {
        *avg_disks_per_set = set_count > 0 ? (double)disk_count / set_count : 0.0;
    }
}
