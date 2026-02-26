/**
 * Peer Grid Implementation
 * 
 * Maintains a list of peers in the cluster for discovery and communication.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <uuid/uuid.h>

#include "buckets.h"
#include "buckets_net.h"

/* ===================================================================
 * Peer Grid Structure
 * ===================================================================*/

#define MAX_PEERS 1000  /* Maximum peers in grid */

/**
 * Internal peer structure (includes storage for buckets_peer_t)
 */
typedef struct peer_entry {
    buckets_peer_t peer;           /* Public peer data */
    struct peer_entry *next;       /* Next in linked list */
} peer_entry_t;

/**
 * Peer grid
 */
struct buckets_peer_grid {
    peer_entry_t *peers;           /* Linked list of peers */
    int count;                     /* Number of peers */
    pthread_mutex_t lock;          /* Thread safety */
};

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/**
 * Generate UUID node ID
 */
static void generate_node_id(char *node_id, size_t len)
{
    (void)len;  /* Unused - UUID string is always 37 bytes */
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, node_id);
}

/**
 * Parse endpoint to extract host and port
 */
static int parse_endpoint(const char *endpoint, char *host, size_t host_len, int *port)
{
    /* Expected format: http://host:port or https://host:port */
    const char *proto_end = strstr(endpoint, "://");
    if (!proto_end) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    const char *host_start = proto_end + 3;
    const char *port_start = strchr(host_start, ':');
    if (!port_start) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Extract host */
    size_t host_length = port_start - host_start;
    if (host_length >= host_len) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    memcpy(host, host_start, host_length);
    host[host_length] = '\0';
    
    /* Extract port */
    *port = atoi(port_start + 1);
    if (*port <= 0 || *port > 65535) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    return BUCKETS_OK;
}

/* ===================================================================
 * Peer Grid API
 * ===================================================================*/

buckets_peer_grid_t* buckets_peer_grid_create(void)
{
    buckets_peer_grid_t *grid = buckets_calloc(1, sizeof(buckets_peer_grid_t));
    if (!grid) {
        return NULL;
    }
    
    grid->peers = NULL;
    grid->count = 0;
    
    pthread_mutex_init(&grid->lock, NULL);
    
    buckets_debug("Created peer grid");
    
    return grid;
}

int buckets_peer_grid_add_peer(buckets_peer_grid_t *grid,
                                const char *endpoint)
{
    if (!grid || !endpoint) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&grid->lock);
    
    /* Check if we've reached max peers */
    if (grid->count >= MAX_PEERS) {
        pthread_mutex_unlock(&grid->lock);
        buckets_error("Peer grid is full (max=%d)", MAX_PEERS);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Validate endpoint format */
    char host[256];
    int port;
    if (parse_endpoint(endpoint, host, sizeof(host), &port) != BUCKETS_OK) {
        pthread_mutex_unlock(&grid->lock);
        buckets_error("Invalid endpoint format: %s", endpoint);
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Check if peer already exists */
    peer_entry_t *cur = grid->peers;
    while (cur) {
        if (strcmp(cur->peer.endpoint, endpoint) == 0) {
            pthread_mutex_unlock(&grid->lock);
            buckets_debug("Peer already exists: %s", endpoint);
            return BUCKETS_OK;
        }
        cur = cur->next;
    }
    
    /* Allocate new peer entry */
    peer_entry_t *entry = buckets_calloc(1, sizeof(peer_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&grid->lock);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Initialize peer data */
    generate_node_id(entry->peer.node_id, sizeof(entry->peer.node_id));
    strncpy(entry->peer.endpoint, endpoint, sizeof(entry->peer.endpoint) - 1);
    entry->peer.online = true;  /* Assume online initially */
    entry->peer.last_seen = time(NULL);
    
    /* Add to list */
    entry->next = grid->peers;
    grid->peers = entry;
    grid->count++;
    
    pthread_mutex_unlock(&grid->lock);
    
    buckets_info("Added peer to grid: %s (node_id=%s)", endpoint, entry->peer.node_id);
    
    return BUCKETS_OK;
}

int buckets_peer_grid_remove_peer(buckets_peer_grid_t *grid,
                                   const char *node_id)
{
    if (!grid || !node_id) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&grid->lock);
    
    peer_entry_t *cur = grid->peers;
    peer_entry_t *prev = NULL;
    
    while (cur) {
        if (strcmp(cur->peer.node_id, node_id) == 0) {
            /* Remove from list */
            if (prev) {
                prev->next = cur->next;
            } else {
                grid->peers = cur->next;
            }
            
            buckets_info("Removed peer from grid: %s (node_id=%s)", 
                        cur->peer.endpoint, cur->peer.node_id);
            
            buckets_free(cur);
            grid->count--;
            
            pthread_mutex_unlock(&grid->lock);
            return BUCKETS_OK;
        }
        
        prev = cur;
        cur = cur->next;
    }
    
    pthread_mutex_unlock(&grid->lock);
    
    buckets_warn("Peer not found: %s", node_id);
    return BUCKETS_ERR_INVALID_ARG;
}

buckets_peer_t** buckets_peer_grid_get_peers(buckets_peer_grid_t *grid,
                                              int *count)
{
    if (!grid || !count) {
        return NULL;
    }
    
    pthread_mutex_lock(&grid->lock);
    
    if (grid->count == 0) {
        *count = 0;
        pthread_mutex_unlock(&grid->lock);
        return NULL;
    }
    
    /* Allocate array of peer pointers */
    buckets_peer_t **peers = buckets_calloc(grid->count, sizeof(buckets_peer_t*));
    if (!peers) {
        pthread_mutex_unlock(&grid->lock);
        return NULL;
    }
    
    /* Fill array */
    int i = 0;
    peer_entry_t *cur = grid->peers;
    while (cur && i < grid->count) {
        peers[i++] = &cur->peer;
        cur = cur->next;
    }
    
    *count = i;
    
    pthread_mutex_unlock(&grid->lock);
    
    return peers;
}

buckets_peer_t* buckets_peer_grid_get_peer(buckets_peer_grid_t *grid,
                                            const char *node_id)
{
    if (!grid || !node_id) {
        return NULL;
    }
    
    pthread_mutex_lock(&grid->lock);
    
    peer_entry_t *cur = grid->peers;
    while (cur) {
        if (strcmp(cur->peer.node_id, node_id) == 0) {
            buckets_peer_t *peer = &cur->peer;
            pthread_mutex_unlock(&grid->lock);
            return peer;
        }
        cur = cur->next;
    }
    
    pthread_mutex_unlock(&grid->lock);
    
    return NULL;
}

int buckets_peer_grid_update_last_seen(buckets_peer_grid_t *grid,
                                        const char *node_id,
                                        time_t timestamp)
{
    if (!grid || !node_id) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&grid->lock);
    
    peer_entry_t *cur = grid->peers;
    while (cur) {
        if (strcmp(cur->peer.node_id, node_id) == 0) {
            cur->peer.last_seen = timestamp;
            cur->peer.online = true;
            pthread_mutex_unlock(&grid->lock);
            return BUCKETS_OK;
        }
        cur = cur->next;
    }
    
    pthread_mutex_unlock(&grid->lock);
    
    return BUCKETS_ERR_INVALID_ARG;
}

void buckets_peer_grid_free(buckets_peer_grid_t *grid)
{
    if (!grid) {
        return;
    }
    
    pthread_mutex_lock(&grid->lock);
    
    /* Free all peers */
    peer_entry_t *cur = grid->peers;
    while (cur) {
        peer_entry_t *next = cur->next;
        buckets_free(cur);
        cur = next;
    }
    
    grid->peers = NULL;
    grid->count = 0;
    
    pthread_mutex_unlock(&grid->lock);
    
    pthread_mutex_destroy(&grid->lock);
    
    buckets_debug("Freed peer grid");
    
    buckets_free(grid);
}
