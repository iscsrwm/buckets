/**
 * Object Placement - Consistent Hashing
 * 
 * Implements consistent hashing with virtual nodes for distributing objects
 * across erasure sets with minimal data movement during topology changes.
 * 
 * Consistent Hashing Algorithm:
 * 1. Build virtual node ring from topology (150 vnodes per set)
 * 2. Hash object path (bucket + "/" + object) using SipHash
 * 3. Binary search ring for next vnode >= hash
 * 4. Return that vnode's pool/set topology information
 * 
 * This provides:
 * - Deterministic placement (same object always goes to same set)
 * - Even distribution across sets (~1% variance)
 * - Minimal migration (~20% when adding/removing sets, not 100%)
 * - Fast computation (O(log N) lookup, ~14 comparisons for 100 sets)
 * 
 * Reference: architecture/SCALE_AND_DATA_PLACEMENT.md Section 6.2
 */

#ifndef BUCKETS_PLACEMENT_H
#define BUCKETS_PLACEMENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "buckets.h"
#include "buckets_cluster.h"

/**
 * Virtual node on hash ring
 * 
 * Each erasure set has multiple virtual nodes distributed around
 * the ring for better load balancing (default: 150 vnodes per set).
 */
typedef struct {
    u64 hash;                   /* Position on ring (hash value) */
    u32 pool_idx;               /* Pool index this vnode belongs to */
    u32 set_idx;                /* Set index this vnode belongs to */
} buckets_placement_vnode_t;

/**
 * Placement result
 * 
 * Contains all information needed to store/retrieve an object
 */
typedef struct {
    /* Set location */
    u32 pool_idx;               /* Pool index */
    u32 set_idx;                /* Set index within pool */
    
    /* Disk information */
    u32 disk_count;             /* Number of disks in set */
    char **disk_paths;          /* Array of disk paths */
    char **disk_uuids;          /* Array of disk UUIDs */
    char **disk_endpoints;      /* Array of full disk endpoints (e.g., "http://node1:9000/mnt/disk1") */
    
    /* Metadata */
    u64 generation;             /* Topology generation number */
    buckets_set_state_t state;  /* Set state (active, draining, removed) */
    
    /* Debug info */
    u64 object_hash;            /* Hash value used for placement */
    u32 vnode_index;            /* Index of vnode selected on ring */
} buckets_placement_result_t;

/**
 * Initialize placement system
 * 
 * Must be called before using placement functions.
 * Uses topology manager to get current cluster topology.
 * Builds virtual node ring for consistent hashing.
 * 
 * @return 0 on success, -1 on error
 */
int buckets_placement_init(void);

/**
 * Rebuild hash ring from current topology
 * 
 * Called when topology changes (add/remove sets).
 * Rebuilds the virtual node ring.
 * 
 * @return 0 on success, -1 on error
 */
int buckets_placement_rebuild_ring(void);

/**
 * Cleanup placement system
 */
void buckets_placement_cleanup(void);

/**
 * Compute object placement using Consistent Hashing
 * 
 * Determines which erasure set should store this object using
 * consistent hashing with virtual nodes.
 * 
 * Algorithm:
 * 1. Hash object path with SipHash
 * 2. Binary search ring for next vnode >= hash
 * 3. Return vnode's pool/set information
 * 
 * @param bucket Bucket name
 * @param object Object key
 * @param result Output placement result (caller must free with buckets_placement_free_result)
 * @return 0 on success, -1 on error
 * 
 * Example:
 *   buckets_placement_result_t *placement;
 *   if (buckets_placement_compute("mybucket", "photos/cat.jpg", &placement) == 0) {
 *       printf("Object goes to pool %u, set %u (vnode %u)\n", 
 *              placement->pool_idx, placement->set_idx, placement->vnode_index);
 *       buckets_placement_free_result(placement);
 *   }
 */
int buckets_placement_compute(const char *bucket, const char *object,
                              buckets_placement_result_t **result);

/**
 * Free placement result
 * 
 * @param result Placement result to free
 */
void buckets_placement_free_result(buckets_placement_result_t *result);

/**
 * Get placement statistics
 * 
 * Returns information about placement distribution.
 * 
 * @param total_sets Output: total number of active sets
 * @param total_disks Output: total number of disks
 * @param avg_disks_per_set Output: average disks per set
 */
void buckets_placement_get_stats(u32 *total_sets, u32 *total_disks, double *avg_disks_per_set);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_PLACEMENT_H */
