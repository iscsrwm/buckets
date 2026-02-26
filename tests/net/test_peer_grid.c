/**
 * Peer Grid Tests
 * 
 * Tests for peer discovery and management using Criterion framework.
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "buckets.h"
#include "buckets_net.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

static buckets_peer_grid_t *grid = NULL;

void setup(void)
{
    buckets_init();
}

void teardown(void)
{
    if (grid) {
        buckets_peer_grid_free(grid);
        grid = NULL;
    }
    buckets_cleanup();
}

TestSuite(peer_grid, .init = setup, .fini = teardown);

/* ===================================================================
 * Tests
 * ===================================================================*/

Test(peer_grid, create_grid)
{
    grid = buckets_peer_grid_create();
    cr_assert_not_null(grid, "Grid should be created");
    
    int count = 0;
    buckets_peer_t **peers = buckets_peer_grid_get_peers(grid, &count);
    cr_assert_eq(count, 0, "New grid should have 0 peers");
    cr_assert_null(peers, "Empty grid should return NULL");
}

Test(peer_grid, add_peer)
{
    grid = buckets_peer_grid_create();
    cr_assert_not_null(grid);
    
    int ret = buckets_peer_grid_add_peer(grid, "http://192.168.1.10:9000");
    cr_assert_eq(ret, BUCKETS_OK, "Should add peer successfully");
    
    int count = 0;
    buckets_peer_t **peers = buckets_peer_grid_get_peers(grid, &count);
    cr_assert_eq(count, 1, "Grid should have 1 peer");
    cr_assert_not_null(peers);
    
    /* Verify peer data */
    cr_assert_str_eq(peers[0]->endpoint, "http://192.168.1.10:9000");
    cr_assert(peers[0]->online, "Peer should be online initially");
    cr_assert_not_null(peers[0]->node_id, "Should have node ID");
    cr_assert_gt(strlen(peers[0]->node_id), 0, "Node ID should not be empty");
    
    buckets_free(peers);
}

Test(peer_grid, add_multiple_peers)
{
    grid = buckets_peer_grid_create();
    cr_assert_not_null(grid);
    
    buckets_peer_grid_add_peer(grid, "http://192.168.1.10:9000");
    buckets_peer_grid_add_peer(grid, "http://192.168.1.11:9000");
    buckets_peer_grid_add_peer(grid, "http://192.168.1.12:9000");
    
    int count = 0;
    buckets_peer_t **peers = buckets_peer_grid_get_peers(grid, &count);
    cr_assert_eq(count, 3, "Grid should have 3 peers");
    
    buckets_free(peers);
}

Test(peer_grid, add_duplicate_peer)
{
    grid = buckets_peer_grid_create();
    cr_assert_not_null(grid);
    
    int ret = buckets_peer_grid_add_peer(grid, "http://192.168.1.10:9000");
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Try to add same peer again */
    ret = buckets_peer_grid_add_peer(grid, "http://192.168.1.10:9000");
    cr_assert_eq(ret, BUCKETS_OK, "Should handle duplicate gracefully");
    
    int count = 0;
    buckets_peer_t **peers = buckets_peer_grid_get_peers(grid, &count);
    cr_assert_eq(count, 1, "Should still have only 1 peer");
    
    buckets_free(peers);
}

Test(peer_grid, remove_peer)
{
    grid = buckets_peer_grid_create();
    cr_assert_not_null(grid);
    
    buckets_peer_grid_add_peer(grid, "http://192.168.1.10:9000");
    buckets_peer_grid_add_peer(grid, "http://192.168.1.11:9000");
    
    int count = 0;
    buckets_peer_t **peers = buckets_peer_grid_get_peers(grid, &count);
    cr_assert_eq(count, 2);
    
    /* Remove first peer */
    char node_id[64];
    strncpy(node_id, peers[0]->node_id, sizeof(node_id));
    buckets_free(peers);
    
    int ret = buckets_peer_grid_remove_peer(grid, node_id);
    cr_assert_eq(ret, BUCKETS_OK, "Should remove peer successfully");
    
    peers = buckets_peer_grid_get_peers(grid, &count);
    cr_assert_eq(count, 1, "Should have 1 peer left");
    
    buckets_free(peers);
}

Test(peer_grid, get_peer_by_id)
{
    grid = buckets_peer_grid_create();
    cr_assert_not_null(grid);
    
    buckets_peer_grid_add_peer(grid, "http://192.168.1.10:9000");
    
    int count = 0;
    buckets_peer_t **peers = buckets_peer_grid_get_peers(grid, &count);
    cr_assert_eq(count, 1);
    
    const char *node_id = peers[0]->node_id;
    buckets_free(peers);
    
    /* Get peer by ID */
    buckets_peer_t *peer = buckets_peer_grid_get_peer(grid, node_id);
    cr_assert_not_null(peer, "Should find peer by ID");
    cr_assert_str_eq(peer->endpoint, "http://192.168.1.10:9000");
}

Test(peer_grid, update_last_seen)
{
    grid = buckets_peer_grid_create();
    cr_assert_not_null(grid);
    
    buckets_peer_grid_add_peer(grid, "http://192.168.1.10:9000");
    
    int count = 0;
    buckets_peer_t **peers = buckets_peer_grid_get_peers(grid, &count);
    cr_assert_eq(count, 1);
    
    const char *node_id = peers[0]->node_id;
    time_t old_time = peers[0]->last_seen;
    buckets_free(peers);
    
    /* Update last seen to future time */
    time_t new_time = time(NULL) + 10;
    int ret = buckets_peer_grid_update_last_seen(grid, node_id, new_time);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Verify update */
    peers = buckets_peer_grid_get_peers(grid, &count);
    cr_assert_gt(peers[0]->last_seen, old_time, "Last seen should be updated");
    
    buckets_free(peers);
}

Test(peer_grid, invalid_endpoint_format)
{
    grid = buckets_peer_grid_create();
    cr_assert_not_null(grid);
    
    /* Missing protocol */
    int ret = buckets_peer_grid_add_peer(grid, "192.168.1.10:9000");
    cr_assert_neq(ret, BUCKETS_OK, "Should fail without protocol");
    
    /* Missing port */
    ret = buckets_peer_grid_add_peer(grid, "http://192.168.1.10");
    cr_assert_neq(ret, BUCKETS_OK, "Should fail without port");
    
    /* Invalid format */
    ret = buckets_peer_grid_add_peer(grid, "not-an-endpoint");
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with invalid format");
}

Test(peer_grid, invalid_args)
{
    grid = buckets_peer_grid_create();
    cr_assert_not_null(grid);
    
    /* NULL grid */
    int ret = buckets_peer_grid_add_peer(NULL, "http://192.168.1.10:9000");
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* NULL endpoint */
    ret = buckets_peer_grid_add_peer(grid, NULL);
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* get_peers with NULL count */
    buckets_peer_t **peers = buckets_peer_grid_get_peers(grid, NULL);
    cr_assert_null(peers);
}

Test(peer_grid, peer_initial_state)
{
    grid = buckets_peer_grid_create();
    cr_assert_not_null(grid);
    
    buckets_peer_grid_add_peer(grid, "https://192.168.1.10:9443");
    
    int count = 0;
    buckets_peer_t **peers = buckets_peer_grid_get_peers(grid, &count);
    cr_assert_eq(count, 1);
    
    /* Verify initial state */
    cr_assert(peers[0]->online, "Should be online initially");
    cr_assert_leq(time(NULL) - peers[0]->last_seen, 1, 
                  "Last seen should be recent");
    cr_assert_str_eq(peers[0]->endpoint, "https://192.168.1.10:9443");
    
    buckets_free(peers);
}
