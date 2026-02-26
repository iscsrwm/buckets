/**
 * Connection Pool Implementation
 * 
 * Manages a pool of reusable TCP connections for peer-to-peer RPC.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <pthread.h>

#include "buckets.h"
#include "buckets_net.h"

/* ===================================================================
 * Connection Structure
 * ===================================================================*/

#define CONN_TIMEOUT_SEC 30  /* Connection idle timeout */

/**
 * Single connection
 */
struct buckets_connection {
    int fd;                      /* Socket file descriptor */
    char host[256];              /* Target host */
    int port;                    /* Target port */
    bool in_use;                 /* Connection in use flag */
    time_t last_used;            /* Last used timestamp */
    struct buckets_connection *next;  /* Next in list */
};

/**
 * Connection pool
 */
struct buckets_conn_pool {
    buckets_connection_t *connections;  /* Linked list of connections */
    int max_conns;                       /* Maximum connections (0=unlimited) */
    int total_conns;                     /* Total connections */
    int active_conns;                    /* Active (in-use) connections */
    pthread_mutex_t lock;                /* Thread safety */
};

/* ===================================================================
 * Internal Helpers
 * ===================================================================*/

/**
 * Create new TCP connection
 */
static int create_connection(const char *host, int port)
{
    struct hostent *server;
    struct sockaddr_in server_addr;
    int sockfd;
    
    /* Create socket */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        buckets_error("Failed to create socket: %s", strerror(errno));
        return -1;
    }
    
    /* Resolve hostname */
    server = gethostbyname(host);
    if (server == NULL) {
        buckets_error("Failed to resolve host %s", host);
        close(sockfd);
        return -1;
    }
    
    /* Connect to server */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    server_addr.sin_port = htons(port);
    
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        buckets_error("Failed to connect to %s:%d: %s", host, port, strerror(errno));
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

/**
 * Check if connection is still alive
 */
static bool is_connection_alive(int fd)
{
    char buf[1];
    ssize_t result = recv(fd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
    
    if (result == 0) {
        /* Connection closed by peer */
        return false;
    }
    
    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        /* Error on socket */
        return false;
    }
    
    return true;
}

/* ===================================================================
 * Connection Pool API
 * ===================================================================*/

buckets_conn_pool_t* buckets_conn_pool_create(int max_conns)
{
    buckets_conn_pool_t *pool = buckets_calloc(1, sizeof(buckets_conn_pool_t));
    if (!pool) {
        return NULL;
    }
    
    pool->connections = NULL;
    pool->max_conns = max_conns;
    pool->total_conns = 0;
    pool->active_conns = 0;
    
    pthread_mutex_init(&pool->lock, NULL);
    
    buckets_debug("Created connection pool (max_conns=%d)", max_conns);
    
    return pool;
}

int buckets_conn_pool_get(buckets_conn_pool_t *pool,
                           const char *host,
                           int port,
                           buckets_connection_t **conn)
{
    if (!pool || !host || port <= 0 || !conn) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    /* Try to find an existing idle connection to the same host:port */
    buckets_connection_t *cur = pool->connections;
    buckets_connection_t *prev = NULL;
    
    while (cur) {
        if (!cur->in_use && 
            strcmp(cur->host, host) == 0 && 
            cur->port == port) {
            
            /* Check if connection is still alive */
            if (is_connection_alive(cur->fd)) {
                /* Reuse this connection */
                cur->in_use = true;
                cur->last_used = time(NULL);
                pool->active_conns++;
                *conn = cur;
                
                pthread_mutex_unlock(&pool->lock);
                buckets_debug("Reused connection to %s:%d (fd=%d)", host, port, cur->fd);
                return BUCKETS_OK;
            } else {
                /* Connection is dead, remove it */
                buckets_debug("Removing dead connection to %s:%d (fd=%d)", host, port, cur->fd);
                close(cur->fd);
                
                if (prev) {
                    prev->next = cur->next;
                } else {
                    pool->connections = cur->next;
                }
                
                buckets_connection_t *to_free = cur;
                cur = cur->next;
                buckets_free(to_free);
                pool->total_conns--;
                continue;
            }
        }
        
        prev = cur;
        cur = cur->next;
    }
    
    /* Check if we can create a new connection */
    if (pool->max_conns > 0 && pool->total_conns >= pool->max_conns) {
        pthread_mutex_unlock(&pool->lock);
        buckets_error("Connection pool limit reached (%d)", pool->max_conns);
        return BUCKETS_ERR_NOMEM;
    }
    
    /* Create new connection */
    int fd = create_connection(host, port);
    if (fd < 0) {
        pthread_mutex_unlock(&pool->lock);
        return BUCKETS_ERR_IO;
    }
    
    /* Allocate connection structure */
    buckets_connection_t *new_conn = buckets_calloc(1, sizeof(buckets_connection_t));
    if (!new_conn) {
        close(fd);
        pthread_mutex_unlock(&pool->lock);
        return BUCKETS_ERR_NOMEM;
    }
    
    new_conn->fd = fd;
    strncpy(new_conn->host, host, sizeof(new_conn->host) - 1);
    new_conn->port = port;
    new_conn->in_use = true;
    new_conn->last_used = time(NULL);
    new_conn->next = pool->connections;
    
    pool->connections = new_conn;
    pool->total_conns++;
    pool->active_conns++;
    
    *conn = new_conn;
    
    pthread_mutex_unlock(&pool->lock);
    
    buckets_debug("Created new connection to %s:%d (fd=%d)", host, port, fd);
    
    return BUCKETS_OK;
}

int buckets_conn_pool_release(buckets_conn_pool_t *pool,
                               buckets_connection_t *conn)
{
    if (!pool || !conn) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    if (!conn->in_use) {
        pthread_mutex_unlock(&pool->lock);
        buckets_warn("Connection already released");
        return BUCKETS_OK;
    }
    
    conn->in_use = false;
    conn->last_used = time(NULL);
    pool->active_conns--;
    
    pthread_mutex_unlock(&pool->lock);
    
    buckets_debug("Released connection to %s:%d (fd=%d)", conn->host, conn->port, conn->fd);
    
    return BUCKETS_OK;
}

int buckets_conn_pool_close(buckets_conn_pool_t *pool,
                             buckets_connection_t *conn)
{
    if (!pool || !conn) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    /* Find and remove connection from list */
    buckets_connection_t *cur = pool->connections;
    buckets_connection_t *prev = NULL;
    
    while (cur) {
        if (cur == conn) {
            /* Remove from list */
            if (prev) {
                prev->next = cur->next;
            } else {
                pool->connections = cur->next;
            }
            
            /* Close socket */
            close(cur->fd);
            
            /* Update counters */
            pool->total_conns--;
            if (cur->in_use) {
                pool->active_conns--;
            }
            
            buckets_debug("Closed connection to %s:%d (fd=%d)", cur->host, cur->port, cur->fd);
            
            buckets_free(cur);
            
            pthread_mutex_unlock(&pool->lock);
            return BUCKETS_OK;
        }
        
        prev = cur;
        cur = cur->next;
    }
    
    pthread_mutex_unlock(&pool->lock);
    
    buckets_warn("Connection not found in pool");
    return BUCKETS_ERR_INVALID_ARG;
}

int buckets_conn_pool_stats(buckets_conn_pool_t *pool,
                             int *total,
                             int *active,
                             int *idle)
{
    if (!pool) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    if (total) *total = pool->total_conns;
    if (active) *active = pool->active_conns;
    if (idle) *idle = pool->total_conns - pool->active_conns;
    
    pthread_mutex_unlock(&pool->lock);
    
    return BUCKETS_OK;
}

void buckets_conn_pool_free(buckets_conn_pool_t *pool)
{
    if (!pool) {
        return;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    /* Close all connections */
    buckets_connection_t *cur = pool->connections;
    while (cur) {
        buckets_connection_t *next = cur->next;
        close(cur->fd);
        buckets_free(cur);
        cur = next;
    }
    
    pool->connections = NULL;
    pool->total_conns = 0;
    pool->active_conns = 0;
    
    pthread_mutex_unlock(&pool->lock);
    
    pthread_mutex_destroy(&pool->lock);
    
    buckets_debug("Freed connection pool");
    
    buckets_free(pool);
}

int buckets_conn_send_request(buckets_connection_t *conn,
                               const char *method,
                               const char *path,
                               const char *body,
                               size_t body_len,
                               char **response,
                               int *status_code)
{
    if (!conn || !method || !path) {
        return BUCKETS_ERR_INVALID_ARG;
    }
    
    /* Build HTTP request */
    char request[4096];
    int len;
    
    if (body && body_len > 0) {
        len = snprintf(request, sizeof(request),
                      "%s %s HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: keep-alive\r\n"
                      "\r\n"
                      "%.*s",
                      method, path, conn->host, conn->port, body_len,
                      (int)body_len, body);
    } else {
        len = snprintf(request, sizeof(request),
                      "%s %s HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "Connection: keep-alive\r\n"
                      "\r\n",
                      method, path, conn->host, conn->port);
    }
    
    /* Send request */
    ssize_t sent = send(conn->fd, request, len, 0);
    if (sent < 0) {
        buckets_error("Failed to send request: %s", strerror(errno));
        return BUCKETS_ERR_IO;
    }
    
    /* Receive response */
    char buffer[8192];
    ssize_t received = recv(conn->fd, buffer, sizeof(buffer) - 1, 0);
    if (received < 0) {
        buckets_error("Failed to receive response: %s", strerror(errno));
        return BUCKETS_ERR_IO;
    }
    
    buffer[received] = '\0';
    
    /* Parse status code */
    if (status_code) {
        *status_code = 0;
        char *status_line = strstr(buffer, "HTTP/1.");
        if (status_line) {
            sscanf(status_line, "HTTP/1.%*d %d", status_code);
        }
    }
    
    /* Find body (after \r\n\r\n) */
    if (response) {
        char *body_start = strstr(buffer, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            *response = buckets_strdup(body_start);
        } else {
            *response = buckets_strdup(buffer);
        }
    }
    
    return BUCKETS_OK;
}
