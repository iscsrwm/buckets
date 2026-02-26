/**
 * Health Checker Implementation
 * 
 * Periodic heartbeat health checking for peer monitoring.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include "buckets.h"
#include "buckets_net.h"

/* ===================================================================
 * Health Checker Structure
 * ===================================================================*/

#define HEALTH_CHECK_PATH "/health"
#define HEALTH_TIMEOUT_SEC 30  /* Mark offline after 30s without heartbeat */

/**
 * Health checker
 */
struct buckets_health_checker {
    buckets_peer_grid_t *grid;           /* Peer grid to monitor */
    buckets_conn_pool_t *pool;           /* Connection pool for heartbeats */
    int interval_sec;                     /* Heartbeat interval */
    
    buckets_health_callback_t callback;   /* Status change callback */
    void *callback_data;                  /* User data for callback */
    
    pthread_t thread;                     /* Background thread */
    bool running;                         /* Running flag */
    pthread_mutex_t lock;                 /* Thread safety */
};

/* ===================================================================
 * Internal Functions
 * ===================================================================*/

/**
 * Send heartbeat to peer
 */
static bool send_heartbeat(buckets_health_checker_t *checker,
                           buckets_peer_t *peer)
{
    /* Parse endpoint */
    char host[256];
    int port;
    
    const char *proto_end = strstr(peer->endpoint, "://");
    if (!proto_end) {
        return false;
    }
    
    const char *host_start = proto_end + 3;
    const char *port_start = strchr(host_start, ':');
    if (!port_start) {
        return false;
    }
    
    size_t host_length = port_start - host_start;
    if (host_length >= sizeof(host)) {
        return false;
    }
    
    memcpy(host, host_start, host_length);
    host[host_length] = '\0';
    port = atoi(port_start + 1);
    
    /* Get connection from pool */
    buckets_connection_t *conn = NULL;
    if (buckets_conn_pool_get(checker->pool, host, port, &conn) != BUCKETS_OK) {
        buckets_debug("Failed to get connection for peer: %s", peer->endpoint);
        return false;
    }
    
    /* Send health check request */
    char *response = NULL;
    int status_code = 0;
    int ret = buckets_conn_send_request(conn, "GET", HEALTH_CHECK_PATH,
                                        NULL, 0, &response, &status_code);
    
    /* Release connection back to pool */
    buckets_conn_pool_release(checker->pool, conn);
    
    if (response) {
        buckets_free(response);
    }
    
    /* Success if we got a response (any status code) */
    if (ret == BUCKETS_OK && status_code > 0) {
        buckets_debug("Heartbeat success for peer: %s (status=%d)", 
                     peer->endpoint, status_code);
        return true;
    }
    
    buckets_debug("Heartbeat failed for peer: %s", peer->endpoint);
    return false;
}

/**
 * Check all peers
 */
static void check_all_peers(buckets_health_checker_t *checker)
{
    int count = 0;
    buckets_peer_t **peers = buckets_peer_grid_get_peers(checker->grid, &count);
    
    if (!peers || count == 0) {
        return;
    }
    
    time_t now = time(NULL);
    
    for (int i = 0; i < count; i++) {
        buckets_peer_t *peer = peers[i];
        
        /* Send heartbeat */
        bool success = send_heartbeat(checker, peer);
        
        bool was_online = peer->online;
        bool is_online = success;
        
        /* Update peer status */
        if (success) {
            buckets_peer_grid_update_last_seen(checker->grid, peer->node_id, now);
        } else {
            /* Check if peer has timed out */
            if (now - peer->last_seen > HEALTH_TIMEOUT_SEC) {
                peer->online = false;
                is_online = false;
            }
        }
        
        /* Call callback if status changed */
        if (was_online != is_online && checker->callback) {
            pthread_mutex_lock(&checker->lock);
            buckets_health_callback_t cb = checker->callback;
            void *data = checker->callback_data;
            pthread_mutex_unlock(&checker->lock);
            
            if (cb) {
                cb(peer->node_id, is_online, data);
            }
        }
    }
    
    buckets_free(peers);
}

/**
 * Health checker thread main function
 */
static void* health_checker_thread_main(void *arg)
{
    buckets_health_checker_t *checker = (buckets_health_checker_t*)arg;
    
    buckets_debug("Health checker thread started (interval=%ds)", checker->interval_sec);
    
    while (checker->running) {
        check_all_peers(checker);
        sleep(checker->interval_sec);
    }
    
    buckets_debug("Health checker thread stopped");
    
    return NULL;
}

/* ===================================================================
 * Health Checker API
 * ===================================================================*/

buckets_health_checker_t* buckets_health_checker_create(
    buckets_peer_grid_t *grid,
    int interval_sec)
{
    if (!grid || interval_sec <= 0) {
        buckets_error("Invalid arguments to health_checker_create");
        return NULL;
    }
    
    buckets_health_checker_t *checker = buckets_calloc(1, sizeof(buckets_health_checker_t));
    if (!checker) {
        return NULL;
    }
    
    checker->grid = grid;
    checker->interval_sec = interval_sec;
    checker->callback = NULL;
    checker->callback_data = NULL;
    checker->running = false;
    
    /* Create connection pool for heartbeats */
    checker->pool = buckets_conn_pool_create(100);  /* Max 100 connections */
    if (!checker->pool) {
        buckets_free(checker);
        return NULL;
    }
    
    pthread_mutex_init(&checker->lock, NULL);
    
    buckets_debug("Created health checker (interval=%ds)", interval_sec);
    
    return checker;
}

int buckets_health_checker_start(buckets_health_checker_t *checker)
{
    if (!checker) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&checker->lock);
    
    if (checker->running) {
        pthread_mutex_unlock(&checker->lock);
        buckets_warn("Health checker already running");
        return BUCKETS_OK;
    }
    
    checker->running = true;
    
    int ret = pthread_create(&checker->thread, NULL, 
                            health_checker_thread_main, checker);
    if (ret != 0) {
        checker->running = false;
        pthread_mutex_unlock(&checker->lock);
        buckets_error("Failed to create health checker thread: %d", ret);
        return BUCKETS_ERR_IO;
    }
    
    pthread_mutex_unlock(&checker->lock);
    
    buckets_info("Health checker started");
    
    return BUCKETS_OK;
}

int buckets_health_checker_stop(buckets_health_checker_t *checker)
{
    if (!checker) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&checker->lock);
    
    if (!checker->running) {
        pthread_mutex_unlock(&checker->lock);
        return BUCKETS_OK;
    }
    
    checker->running = false;
    pthread_mutex_unlock(&checker->lock);
    
    /* Wait for thread to finish */
    pthread_join(checker->thread, NULL);
    
    buckets_info("Health checker stopped");
    
    return BUCKETS_OK;
}

int buckets_health_checker_set_callback(buckets_health_checker_t *checker,
                                         buckets_health_callback_t callback,
                                         void *user_data)
{
    if (!checker) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&checker->lock);
    checker->callback = callback;
    checker->callback_data = user_data;
    pthread_mutex_unlock(&checker->lock);
    
    return BUCKETS_OK;
}

void buckets_health_checker_free(buckets_health_checker_t *checker)
{
    if (!checker) {
        return;
    }
    
    /* Stop if running */
    if (checker->running) {
        buckets_health_checker_stop(checker);
    }
    
    /* Free connection pool */
    if (checker->pool) {
        buckets_conn_pool_free(checker->pool);
    }
    
    pthread_mutex_destroy(&checker->lock);
    
    buckets_debug("Freed health checker");
    
    buckets_free(checker);
}
