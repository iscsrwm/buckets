/**
 * Consistent Hash Ring Implementation
 * 
 * Implements a consistent hash ring with virtual nodes for distributing
 * objects across a dynamic set of storage nodes with minimal rebalancing.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buckets.h"
#include "buckets_hash.h"
#include "buckets_ring.h"

/* Compare function for sorting vnodes by hash */
static int vnode_compare(const void *a, const void *b)
{
    const buckets_vnode_t *va = (const buckets_vnode_t *)a;
    const buckets_vnode_t *vb = (const buckets_vnode_t *)b;
    
    if (va->hash < vb->hash) return -1;
    if (va->hash > vb->hash) return 1;
    return 0;
}

buckets_ring_t *buckets_ring_create(i32 vnodes_per_node, u64 seed)
{
    if (vnodes_per_node < 0) {
        return NULL;
    }
    
    if (vnodes_per_node == 0) {
        vnodes_per_node = BUCKETS_DEFAULT_VNODES;
    }
    
    buckets_ring_t *ring = buckets_calloc(1, sizeof(buckets_ring_t));
    if (!ring) {
        return NULL;
    }
    
    ring->vnodes_per_node = vnodes_per_node;
    ring->seed = seed;
    ring->vnode_count = 0;
    ring->node_count = 0;
    ring->vnodes = NULL;
    
    return ring;
}

void buckets_ring_free(buckets_ring_t *ring)
{
    if (!ring) {
        return;
    }
    
    if (ring->vnodes) {
        for (size_t i = 0; i < ring->vnode_count; i++) {
            buckets_free(ring->vnodes[i].node_name);
        }
        buckets_free(ring->vnodes);
    }
    
    buckets_free(ring);
}

buckets_error_t buckets_ring_add_node(buckets_ring_t *ring,
                                     i32 node_id,
                                     const char *node_name)
{
    if (!ring || !node_name) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Check if node already exists */
    for (size_t i = 0; i < ring->vnode_count; i++) {
        if (ring->vnodes[i].node_id == node_id) {
            buckets_warn("Node %d already exists in ring", node_id);
            return BUCKETS_ERR_EXISTS;
        }
    }
    
    /* Allocate space for new vnodes */
    size_t new_vnode_count = ring->vnode_count + ring->vnodes_per_node;
    buckets_vnode_t *new_vnodes = buckets_realloc(ring->vnodes,
                                                  new_vnode_count * sizeof(buckets_vnode_t));
    if (!new_vnodes) {
        return BUCKETS_ERR_NOMEM;
    }
    
    ring->vnodes = new_vnodes;
    
    /* Create virtual nodes for this physical node */
    for (i32 i = 0; i < ring->vnodes_per_node; i++) {
        /* Generate unique key for this vnode: "node_name:vnode_index" */
        char vnode_key[256];
        snprintf(vnode_key, sizeof(vnode_key), "%s:%d", node_name, i);
        
        /* Hash the vnode key to get position on ring */
        u64 hash = buckets_xxhash64(ring->seed, vnode_key, strlen(vnode_key));
        
        /* Add vnode */
        size_t idx = ring->vnode_count + i;
        ring->vnodes[idx].hash = hash;
        ring->vnodes[idx].node_id = node_id;
        ring->vnodes[idx].node_name = buckets_strdup(node_name);
    }
    
    ring->vnode_count = new_vnode_count;
    ring->node_count++;
    
    /* Sort vnodes by hash for binary search */
    qsort(ring->vnodes, ring->vnode_count, sizeof(buckets_vnode_t), vnode_compare);
    
    buckets_debug("Added node %d (%s) to ring: %d vnodes, total %zu vnodes",
                 node_id, node_name, ring->vnodes_per_node, ring->vnode_count);
    
    return BUCKETS_OK;
}

buckets_error_t buckets_ring_remove_node(buckets_ring_t *ring, i32 node_id)
{
    if (!ring) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Count vnodes to remove */
    size_t remove_count = 0;
    for (size_t i = 0; i < ring->vnode_count; i++) {
        if (ring->vnodes[i].node_id == node_id) {
            remove_count++;
        }
    }
    
    if (remove_count == 0) {
        buckets_warn("Node %d not found in ring", node_id);
        return BUCKETS_ERR_NOT_FOUND;
    }
    
    /* Create new vnode array without the removed node's vnodes */
    size_t new_count = ring->vnode_count - remove_count;
    buckets_vnode_t *new_vnodes = buckets_calloc(new_count, sizeof(buckets_vnode_t));
    if (!new_vnodes && new_count > 0) {
        return BUCKETS_ERR_NOMEM;
    }
    
    size_t j = 0;
    for (size_t i = 0; i < ring->vnode_count; i++) {
        if (ring->vnodes[i].node_id != node_id) {
            new_vnodes[j] = ring->vnodes[i];
            j++;
        } else {
            buckets_free(ring->vnodes[i].node_name);
        }
    }
    
    buckets_free(ring->vnodes);
    ring->vnodes = new_vnodes;
    ring->vnode_count = new_count;
    ring->node_count--;
    
    buckets_debug("Removed node %d from ring: %zu vnodes remaining",
                 node_id, ring->vnode_count);
    
    return BUCKETS_OK;
}

i32 buckets_ring_lookup(const buckets_ring_t *ring, const char *object_name)
{
    if (!ring || !object_name || ring->vnode_count == 0) {
        return -1;
    }
    
    /* Hash the object name */
    u64 hash = buckets_xxhash64(ring->seed, object_name, strlen(object_name));
    
    /* Binary search for the first vnode >= hash (clockwise search) */
    size_t left = 0;
    size_t right = ring->vnode_count;
    
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (ring->vnodes[mid].hash < hash) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    
    /* Wrap around if we're past the end */
    if (left >= ring->vnode_count) {
        left = 0;
    }
    
    return ring->vnodes[left].node_id;
}

size_t buckets_ring_lookup_n(const buckets_ring_t *ring,
                             const char *object_name,
                             size_t n,
                             i32 *out_nodes)
{
    if (!ring || !object_name || !out_nodes || n == 0 || ring->vnode_count == 0) {
        return 0;
    }
    
    /* Hash the object name */
    u64 hash = buckets_xxhash64(ring->seed, object_name, strlen(object_name));
    
    /* Binary search for starting position */
    size_t left = 0;
    size_t right = ring->vnode_count;
    
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (ring->vnodes[mid].hash < hash) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    
    if (left >= ring->vnode_count) {
        left = 0;
    }
    
    /* Find N distinct physical nodes */
    size_t found = 0;
    size_t pos = left;
    bool *seen = buckets_calloc(ring->node_count + 1, sizeof(bool));  /* Track seen node_ids */
    if (!seen) {
        return 0;
    }
    
    /* Walk clockwise around the ring */
    for (size_t i = 0; i < ring->vnode_count && found < n; i++) {
        i32 node_id = ring->vnodes[pos].node_id;
        
        /* Only add if we haven't seen this physical node yet */
        if (!seen[node_id]) {
            out_nodes[found] = node_id;
            seen[node_id] = true;
            found++;
        }
        
        /* Move to next vnode (wrap around) */
        pos = (pos + 1) % ring->vnode_count;
    }
    
    buckets_free(seen);
    return found;
}

void buckets_ring_get_distribution(const buckets_ring_t *ring,
                                  size_t sample_count,
                                  double *out_min,
                                  double *out_max,
                                  double *out_stddev)
{
    if (!ring || ring->node_count == 0 || sample_count == 0) {
        if (out_min) *out_min = 0.0;
        if (out_max) *out_max = 0.0;
        if (out_stddev) *out_stddev = 0.0;
        return;
    }
    
    /* Count objects per node */
    i32 *counts = buckets_calloc(ring->node_count + 1, sizeof(i32));
    if (!counts) {
        return;
    }
    
    /* Sample hash space */
    char key[64];
    for (size_t i = 0; i < sample_count; i++) {
        snprintf(key, sizeof(key), "object-%zu", i);
        i32 node_id = buckets_ring_lookup(ring, key);
        if (node_id >= 0) {
            counts[node_id]++;
        }
    }
    
    /* Calculate statistics */
    double expected = (double)sample_count / ring->node_count;
    double min_pct = 100.0;
    double max_pct = 0.0;
    double sum_sq_diff = 0.0;
    
    for (size_t i = 0; i <= ring->node_count; i++) {
        if (counts[i] > 0) {
            double pct = (counts[i] * 100.0) / expected;
            if (pct < min_pct) min_pct = pct;
            if (pct > max_pct) max_pct = pct;
            
            double diff = counts[i] - expected;
            sum_sq_diff += diff * diff;
        }
    }
    
    double variance = sum_sq_diff / ring->node_count;
    double stddev = sqrt(variance);
    
    if (out_min) *out_min = min_pct;
    if (out_max) *out_max = max_pct;
    if (out_stddev) *out_stddev = stddev;
    
    buckets_free(counts);
}

void buckets_ring_print(const buckets_ring_t *ring)
{
    if (!ring) {
        printf("Ring: NULL\n");
        return;
    }
    
    printf("Ring: %zu vnodes, %zu physical nodes, %d vnodes/node\n",
           ring->vnode_count, ring->node_count, ring->vnodes_per_node);
    
    for (size_t i = 0; i < ring->vnode_count && i < 10; i++) {
        printf("  [%zu] hash=0x%016llx -> node %d (%s)\n",
               i,
               (unsigned long long)ring->vnodes[i].hash,
               ring->vnodes[i].node_id,
               ring->vnodes[i].node_name);
    }
    
    if (ring->vnode_count > 10) {
        printf("  ... (%zu more vnodes)\n", ring->vnode_count - 10);
    }
}

/* ============================================================================
 * Jump Consistent Hash
 * ============================================================================ */

i32 buckets_jump_hash(u64 key, i32 num_buckets)
{
    if (num_buckets <= 0) {
        return -1;
    }
    
    i64 b = -1;
    i64 j = 0;
    
    while (j < num_buckets) {
        b = j;
        key = key * 2862933555777941757ULL + 1;
        j = (i64)((b + 1) * ((double)(1LL << 31) / (double)((key >> 33) + 1)));
    }
    
    return (i32)b;
}

i32 buckets_jump_hash_str(const char *str, i32 num_buckets)
{
    if (!str) {
        return -1;
    }
    
    u64 hash = buckets_xxhash64(0, str, strlen(str));
    return buckets_jump_hash(hash, num_buckets);
}
