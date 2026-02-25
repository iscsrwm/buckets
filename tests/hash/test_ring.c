/**
 * Hash Ring Tests
 * 
 * Test suite for consistent hash ring with virtual nodes.
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <string.h>

#include "buckets.h"
#include "buckets_ring.h"

/* Test: Create and free ring */
Test(ring, create_free)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    cr_assert_not_null(ring);
    buckets_ring_free(ring);
}

/* Test: Add single node */
Test(ring, add_node)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    cr_assert_not_null(ring);
    
    buckets_error_t err = buckets_ring_add_node(ring, 1, "node1");
    cr_assert_eq(err, BUCKETS_OK);
    
    buckets_ring_free(ring);
}

/* Test: Add multiple nodes */
Test(ring, add_multiple_nodes)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    
    buckets_ring_add_node(ring, 1, "node1");
    buckets_ring_add_node(ring, 2, "node2");
    buckets_ring_add_node(ring, 3, "node3");
    
    buckets_ring_free(ring);
}

/* Test: Lookup on empty ring */
Test(ring, lookup_empty_ring)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    
    i32 node = buckets_ring_lookup(ring, "test-object");
    cr_assert_eq(node, -1, "Empty ring should return -1");
    
    buckets_ring_free(ring);
}

/* Test: Lookup with single node */
Test(ring, lookup_single_node)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    buckets_ring_add_node(ring, 1, "node1");
    
    i32 node = buckets_ring_lookup(ring, "test-object");
    cr_assert_eq(node, 1, "Should map to node 1");
    
    buckets_ring_free(ring);
}

/* Test: Lookup deterministic */
Test(ring, lookup_deterministic)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    buckets_ring_add_node(ring, 1, "node1");
    buckets_ring_add_node(ring, 2, "node2");
    buckets_ring_add_node(ring, 3, "node3");
    
    const char *obj = "my-bucket/my-object";
    i32 node1 = buckets_ring_lookup(ring, obj);
    i32 node2 = buckets_ring_lookup(ring, obj);
    i32 node3 = buckets_ring_lookup(ring, obj);
    
    cr_assert_eq(node1, node2, "Lookups should be deterministic");
    cr_assert_eq(node2, node3, "Lookups should be deterministic");
    
    buckets_ring_free(ring);
}

/* Test: Remove node */
Test(ring, remove_node)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    buckets_ring_add_node(ring, 1, "node1");
    buckets_ring_add_node(ring, 2, "node2");
    
    buckets_error_t err = buckets_ring_remove_node(ring, 1);
    cr_assert_eq(err, BUCKETS_OK);
    
    /* All objects should now map to node 2 */
    i32 node = buckets_ring_lookup(ring, "test");
    cr_assert_eq(node, 2);
    
    buckets_ring_free(ring);
}

/* Test: Remove non-existent node */
Test(ring, remove_nonexistent)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    buckets_ring_add_node(ring, 1, "node1");
    
    buckets_error_t err = buckets_ring_remove_node(ring, 99);
    cr_assert_neq(err, BUCKETS_OK, "Should fail to remove non-existent node");
    
    buckets_ring_free(ring);
}

/* Test: Distribution across nodes */
Test(ring, distribution)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    buckets_ring_add_node(ring, 1, "node1");
    buckets_ring_add_node(ring, 2, "node2");
    buckets_ring_add_node(ring, 3, "node3");
    
    /* Count objects per node */
    i32 counts[4] = {0};
    char obj[64];
    for (int i = 0; i < 10000; i++) {
        snprintf(obj, sizeof(obj), "object-%d", i);
        i32 node = buckets_ring_lookup(ring, obj);
        cr_assert_geq(node, 1);
        cr_assert_leq(node, 3);
        counts[node]++;
    }
    
    /* Each node should get roughly 1/3 of objects (3333 ± tolerance) */
    for (i32 i = 1; i <= 3; i++) {
        cr_assert_gt(counts[i], 2500, "Node %d: %d objects (too few)", i, counts[i]);
        cr_assert_lt(counts[i], 4000, "Node %d: %d objects (too many)", i, counts[i]);
    }
    
    buckets_ring_free(ring);
}

/* Test: Lookup N replicas */
Test(ring, lookup_n_replicas)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    buckets_ring_add_node(ring, 1, "node1");
    buckets_ring_add_node(ring, 2, "node2");
    buckets_ring_add_node(ring, 3, "node3");
    
    i32 nodes[3];
    size_t n = buckets_ring_lookup_n(ring, "test-object", 3, nodes);
    
    cr_assert_eq(n, 3, "Should return 3 replicas");
    
    /* All nodes should be different */
    cr_assert_neq(nodes[0], nodes[1]);
    cr_assert_neq(nodes[1], nodes[2]);
    cr_assert_neq(nodes[0], nodes[2]);
    
    buckets_ring_free(ring);
}

/* Test: Lookup more replicas than nodes */
Test(ring, lookup_too_many_replicas)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    buckets_ring_add_node(ring, 1, "node1");
    buckets_ring_add_node(ring, 2, "node2");
    
    i32 nodes[5];
    size_t n = buckets_ring_lookup_n(ring, "test-object", 5, nodes);
    
    cr_assert_eq(n, 2, "Should return only 2 nodes (all available)");
    
    buckets_ring_free(ring);
}

/* Test: Jump hash basic */
Test(ring, jump_hash_basic)
{
    i32 bucket = buckets_jump_hash(12345, 10);
    cr_assert_geq(bucket, 0);
    cr_assert_lt(bucket, 10);
}

/* Test: Jump hash deterministic */
Test(ring, jump_hash_deterministic)
{
    u64 key = 0x123456789ABCDEFULL;
    i32 b1 = buckets_jump_hash(key, 16);
    i32 b2 = buckets_jump_hash(key, 16);
    i32 b3 = buckets_jump_hash(key, 16);
    
    cr_assert_eq(b1, b2);
    cr_assert_eq(b2, b3);
}

/* Test: Jump hash string */
Test(ring, jump_hash_string)
{
    i32 bucket = buckets_jump_hash_str("test-object", 8);
    cr_assert_geq(bucket, 0);
    cr_assert_lt(bucket, 8);
    
    /* Should be deterministic */
    i32 bucket2 = buckets_jump_hash_str("test-object", 8);
    cr_assert_eq(bucket, bucket2);
}

/* Test: Jump hash distribution */
Test(ring, jump_hash_distribution)
{
    const int num_buckets = 10;
    int counts[10] = {0};
    
    for (int i = 0; i < 10000; i++) {
        i32 bucket = buckets_jump_hash((u64)i, num_buckets);
        counts[bucket]++;
    }
    
    /* Each bucket should get roughly 1000 objects */
    for (int i = 0; i < num_buckets; i++) {
        cr_assert_gt(counts[i], 800, "Bucket %d: %d (too few)", i, counts[i]);
        cr_assert_lt(counts[i], 1200, "Bucket %d: %d (too many)", i, counts[i]);
    }
}

/* Test: Minimal rebalancing on node add */
Test(ring, minimal_rebalancing_add)
{
    buckets_ring_t *ring = buckets_ring_create(150, 0);
    buckets_ring_add_node(ring, 1, "node1");
    buckets_ring_add_node(ring, 2, "node2");
    
    /* Record where 1000 objects map */
    i32 *before = malloc(1000 * sizeof(i32));
    char obj[64];
    for (int i = 0; i < 1000; i++) {
        snprintf(obj, sizeof(obj), "object-%d", i);
        before[i] = buckets_ring_lookup(ring, obj);
    }
    
    /* Add a new node */
    buckets_ring_add_node(ring, 3, "node3");
    
    /* Check how many objects moved */
    int moved = 0;
    for (int i = 0; i < 1000; i++) {
        snprintf(obj, sizeof(obj), "object-%d", i);
        i32 after = buckets_ring_lookup(ring, obj);
        if (before[i] != after) {
            moved++;
        }
    }
    
    /* Should move roughly 1/3 of objects (for 2->3 nodes) */
    /* With virtual nodes, movement should be close to K/(N+1) */
    double pct_moved = (moved * 100.0) / 1000;
    cr_assert_gt(pct_moved, 20.0, "Too few objects moved: %.1f%%", pct_moved);
    cr_assert_lt(pct_moved, 50.0, "Too many objects moved: %.1f%%", pct_moved);
    
    free(before);
    buckets_ring_free(ring);
}

/* Test: NULL input handling */
Test(ring, null_inputs)
{
    cr_assert_null(buckets_ring_create(-1, 0));
    buckets_ring_free(NULL);  /* Should not crash */
    
    cr_assert_eq(buckets_ring_lookup(NULL, "test"), -1);
    cr_assert_eq(buckets_jump_hash_str(NULL, 10), -1);
}
