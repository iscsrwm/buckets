/**
 * Broadcast Implementation
 * 
 * Broadcast RPC calls to all peers in grid.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "buckets.h"
#include "buckets_net.h"
#include "cJSON.h"

/* ===================================================================
 * Broadcast API
 * ===================================================================*/

int buckets_rpc_broadcast(buckets_rpc_context_t *ctx,
                          buckets_peer_grid_t *grid,
                          const char *method,
                          cJSON *params,
                          buckets_broadcast_result_t **result,
                          int timeout_ms)
{
    if (!ctx || !grid || !method || !result) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Get list of peers */
    int peer_count = 0;
    buckets_peer_t **peers = buckets_peer_grid_get_peers(grid, &peer_count);
    
    if (peer_count == 0) {
        /* No peers - return empty result */
        buckets_broadcast_result_t *res = buckets_calloc(1, sizeof(buckets_broadcast_result_t));
        if (!res) {
            return BUCKETS_ERR_NOMEM;
        }
        res->total = 0;
        res->success = 0;
        res->failed = 0;
        res->responses = NULL;
        res->failed_peers = NULL;
        *result = res;
        return BUCKETS_OK;
    }
    
    /* Allocate result structure */
    buckets_broadcast_result_t *res = buckets_calloc(1, sizeof(buckets_broadcast_result_t));
    if (!res) {
        return BUCKETS_ERR_NOMEM;
    }
    
    res->total = peer_count;
    res->success = 0;
    res->failed = 0;
    
    /* Allocate arrays for responses and failed peers (worst case: all succeed or all fail) */
    res->responses = buckets_calloc(peer_count, sizeof(buckets_rpc_response_t*));
    res->failed_peers = buckets_calloc(peer_count, sizeof(char*));
    
    if (!res->responses || !res->failed_peers) {
        buckets_free(res->responses);
        buckets_free(res->failed_peers);
        buckets_free(res);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Call RPC on each peer */
    for (int i = 0; i < peer_count; i++) {
        buckets_peer_t *peer = peers[i];
        
        /* Skip offline peers */
        if (!peer->online) {
            char *failed_endpoint = buckets_malloc(strlen(peer->endpoint) + 1);
            if (failed_endpoint) {
                strcpy(failed_endpoint, peer->endpoint);
                res->failed_peers[res->failed] = failed_endpoint;
                res->failed++;
            }
            continue;
        }
        
        /* Call RPC */
        buckets_rpc_response_t *response = NULL;
        int ret = buckets_rpc_call(ctx, peer->endpoint, method, params,
                                    &response, timeout_ms);
        
        if (ret == BUCKETS_OK && response) {
            /* Success - check if response indicates error */
            if (response->error_code == 0) {
                res->responses[res->success] = response;
                res->success++;
            } else {
                /* RPC succeeded but handler returned error */
                buckets_rpc_response_free(response);
                char *failed_endpoint = buckets_malloc(strlen(peer->endpoint) + 1);
                if (failed_endpoint) {
                    strcpy(failed_endpoint, peer->endpoint);
                    res->failed_peers[res->failed] = failed_endpoint;
                    res->failed++;
                }
            }
        } else {
            /* RPC call failed */
            if (response) {
                buckets_rpc_response_free(response);
            }
            char *failed_endpoint = buckets_malloc(strlen(peer->endpoint) + 1);
            if (failed_endpoint) {
                strcpy(failed_endpoint, peer->endpoint);
                res->failed_peers[res->failed] = failed_endpoint;
                res->failed++;
            }
        }
    }
    
    buckets_debug("Broadcast: %s to %d peers: %d success, %d failed",
                  method, res->total, res->success, res->failed);
    
    *result = res;
    return BUCKETS_OK;
}

void buckets_broadcast_result_free(buckets_broadcast_result_t *result)
{
    if (!result) {
        return;
    }
    
    /* Free successful responses */
    if (result->responses) {
        for (int i = 0; i < result->success; i++) {
            buckets_rpc_response_free(result->responses[i]);
        }
        buckets_free(result->responses);
    }
    
    /* Free failed peer endpoints */
    if (result->failed_peers) {
        for (int i = 0; i < result->failed; i++) {
            buckets_free(result->failed_peers[i]);
        }
        buckets_free(result->failed_peers);
    }
    
    buckets_free(result);
}
