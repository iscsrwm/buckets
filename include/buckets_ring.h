/**
 * Consistent Hashing Ring
 * 
 * This module implements a consistent hash ring with virtual nodes for
 * distributing objects across a dynamic set of storage nodes.
 * 
 * Consistent hashing ensures minimal data movement when nodes are added
 * or removed from the cluster.
 */

#ifndef BUCKETS_RING_H
#define BUCKETS_RING_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buckets.h"

/* Default number of virtual nodes per physical node (150 as per architecture) */
#define BUCKETS_DEFAULT_VNODES 150

/**
 * Virtual node in the hash ring
 * 
 * Each physical node has multiple virtual nodes distributed
 * around the ring for better load balancing.
 */
typedef struct buckets_vnode {
    u64 hash;           /* Position on the ring (hash value) */
    i32 node_id;        /* Physical node ID this vnode belongs to */
    char *node_name;    /* Physical node name (for debugging) */
} buckets_vnode_t;

/**
 * Consistent hash ring structure
 * 
 * Maintains a sorted array of virtual nodes for efficient lookup.
 */
typedef struct buckets_ring {
    buckets_vnode_t *vnodes;    /* Array of virtual nodes (sorted by hash) */
    size_t vnode_count;         /* Number of virtual nodes */
    size_t node_count;          /* Number of physical nodes */
    i32 vnodes_per_node;        /* Virtual nodes per physical node */
    u64 seed;                   /* Hash seed for ring construction */
} buckets_ring_t;

/**
 * Create a new hash ring
 * 
 * @param vnodes_per_node Virtual nodes per physical node (use BUCKETS_DEFAULT_VNODES)
 * @param seed Hash seed (use 0 for default)
 * @return New ring (caller must free with buckets_ring_free)
 */
buckets_ring_t *buckets_ring_create(i32 vnodes_per_node, u64 seed);

/**
 * Free hash ring
 * 
 * @param ring Ring to free
 */
void buckets_ring_free(buckets_ring_t *ring);

/**
 * Add a node to the ring
 * 
 * Creates vnodes_per_node virtual nodes for this physical node
 * and distributes them around the ring.
 * 
 * @param ring Hash ring
 * @param node_id Physical node ID
 * @param node_name Physical node name (for debugging, will be copied)
 * @return BUCKETS_OK on success, error code on failure
 */
buckets_error_t buckets_ring_add_node(buckets_ring_t *ring,
                                     i32 node_id,
                                     const char *node_name);

/**
 * Remove a node from the ring
 * 
 * Removes all virtual nodes belonging to this physical node.
 * 
 * @param ring Hash ring
 * @param node_id Physical node ID to remove
 * @return BUCKETS_OK on success, error code on failure
 */
buckets_error_t buckets_ring_remove_node(buckets_ring_t *ring, i32 node_id);

/**
 * Lookup which node owns an object
 * 
 * Uses consistent hashing to map the object name to a physical node.
 * Returns the first virtual node clockwise from the object's hash position.
 * 
 * @param ring Hash ring
 * @param object_name Object name to lookup
 * @return Physical node ID, or -1 if ring is empty
 */
i32 buckets_ring_lookup(const buckets_ring_t *ring, const char *object_name);

/**
 * Get N successor nodes for an object
 * 
 * Returns the N distinct physical nodes responsible for storing
 * replicas of this object (for replication factor N).
 * 
 * @param ring Hash ring
 * @param object_name Object name
 * @param n Number of replicas
 * @param out_nodes Output array (must have space for n integers)
 * @return Number of nodes returned (may be less than n if not enough nodes)
 */
size_t buckets_ring_lookup_n(const buckets_ring_t *ring,
                             const char *object_name,
                             size_t n,
                             i32 *out_nodes);

/**
 * Get load distribution statistics
 * 
 * Analyzes how objects are distributed across nodes by sampling
 * the hash space.
 * 
 * @param ring Hash ring
 * @param sample_count Number of samples to test
 * @param out_min Output: minimum objects per node (%)
 * @param out_max Output: maximum objects per node (%)
 * @param out_stddev Output: standard deviation
 */
void buckets_ring_get_distribution(const buckets_ring_t *ring,
                                  size_t sample_count,
                                  double *out_min,
                                  double *out_max,
                                  double *out_stddev);

/**
 * Print ring structure (for debugging)
 * 
 * @param ring Hash ring
 */
void buckets_ring_print(const buckets_ring_t *ring);

/* ============================================================================
 * Jump Consistent Hash
 * ============================================================================
 * 
 * A simple, fast consistent hash algorithm that uses no memory.
 * Good for when you know the number of buckets upfront.
 * 
 * Reference: "A Fast, Minimal Memory, Consistent Hash Algorithm" (Google)
 */

/**
 * Jump consistent hash
 * 
 * Maps a 64-bit key to a bucket in the range [0, num_buckets).
 * This is stateless and requires no memory, but doesn't support
 * arbitrary node additions (must rehash when bucket count changes).
 * 
 * @param key Hash key
 * @param num_buckets Number of buckets
 * @return Bucket index [0, num_buckets-1]
 */
i32 buckets_jump_hash(u64 key, i32 num_buckets);

/**
 * Jump consistent hash for strings
 * 
 * Convenience wrapper that hashes the string first.
 * 
 * @param str Input string
 * @param num_buckets Number of buckets
 * @return Bucket index [0, num_buckets-1]
 */
i32 buckets_jump_hash_str(const char *str, i32 num_buckets);

#ifdef __cplusplus
}
#endif

#endif /* BUCKETS_RING_H */
