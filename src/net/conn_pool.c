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
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
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
    
    /* Connect to server with timeout */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    server_addr.sin_port = htons(port);
    
    /* Set socket to non-blocking mode for connect */
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    int connect_result = connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    if (connect_result < 0) {
        if (errno == EINPROGRESS) {
            /* Connection in progress, wait with timeout */
            fd_set write_fds;
            struct timeval timeout;
            timeout.tv_sec = 5;   /* 5 second connect timeout */
            timeout.tv_usec = 0;
            
            FD_ZERO(&write_fds);
            FD_SET(sockfd, &write_fds);
            
            int select_result = select(sockfd + 1, NULL, &write_fds, NULL, &timeout);
            
            if (select_result <= 0) {
                /* Timeout or error */
                buckets_error("Connection to %s:%d timed out or failed", host, port);
                close(sockfd);
                return -1;
            }
            
            /* Check if connection succeeded */
            int sock_error = 0;
            socklen_t len = sizeof(sock_error);
            if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &sock_error, &len) < 0 || sock_error != 0) {
                buckets_error("Failed to connect to %s:%d: %s", host, port, 
                             sock_error ? strerror(sock_error) : "unknown error");
                close(sockfd);
                return -1;
            }
        } else {
            /* Immediate connection error (e.g., connection refused) */
            buckets_error("Failed to connect to %s:%d: %s", host, port, strerror(errno));
            close(sockfd);
            return -1;
        }
    }
    
    /* Set socket back to blocking mode */
    fcntl(sockfd, F_SETFL, flags);
    
    return sockfd;
}

/**
 * Check if connection is still alive
 */
static bool is_connection_alive(int fd)
{
    char buf[1024];
    ssize_t result = recv(fd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
    
    if (result == 0) {
        /* Connection closed by peer */
        return false;
    }
    
    if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        /* Error on socket */
        return false;
    }
    
    /* If there's unexpected data in the buffer, drain it or reject the connection */
    if (result > 0) {
        /* There's data waiting - this shouldn't happen if we properly consumed responses */
        /* Drain up to 1KB of leftover data to try to recover the connection */
        ssize_t drained = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (drained > 0) {
            buckets_warn("Drained %zd bytes of leftover data from connection fd=%d", drained, fd);
        }
        /* After draining, check again if more data exists */
        result = recv(fd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (result > 0) {
            /* Still has data after draining - connection is corrupted */
            buckets_warn("Connection fd=%d still has data after draining, marking as dead", fd);
            return false;
        }
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
    
    /* Build HTTP request headers */
    char headers[1024];
    int header_len;
    
    if (body && body_len > 0) {
        header_len = snprintf(headers, sizeof(headers),
                      "%s %s HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: keep-alive\r\n"
                      "\r\n",
                      method, path, conn->host, conn->port, body_len);
    } else {
        header_len = snprintf(headers, sizeof(headers),
                      "%s %s HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "Connection: keep-alive\r\n"
                      "\r\n",
                      method, path, conn->host, conn->port);
    }
    
    /* Set send timeout to prevent blocking forever on large sends */
    struct timeval send_timeout;
    send_timeout.tv_sec = 120;  /* 120 second timeout for large chunk sends */
    send_timeout.tv_usec = 0;
    if (setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) < 0) {
        buckets_warn("Failed to set send timeout: %s", strerror(errno));
    }
    
    /* Send headers */
    ssize_t sent = send(conn->fd, headers, header_len, 0);
    if (sent < 0) {
        buckets_error("Failed to send request headers: %s", strerror(errno));
        return BUCKETS_ERR_IO;
    }
    
    /* Send body if present - loop to handle large bodies */
    if (body && body_len > 0) {
        size_t total_sent = 0;
        while (total_sent < body_len) {
            ssize_t n = send(conn->fd, body + total_sent, body_len - total_sent, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Timeout or would block - retry */
                    continue;
                }
                buckets_error("Failed to send request body: %s (sent %zu/%zu bytes)", 
                             strerror(errno), total_sent, body_len);
                return BUCKETS_ERR_IO;
            }
            if (n == 0) {
                buckets_error("Connection closed while sending body (sent %zu/%zu bytes)",
                             total_sent, body_len);
                return BUCKETS_ERR_IO;
            }
            total_sent += n;
        }
    }
    
    /* Set receive timeout to prevent hanging - use longer timeout for large transfers */
    struct timeval timeout;
    timeout.tv_sec = 120;  /* 120 second timeout for large responses */
    timeout.tv_usec = 0;
    if (setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        buckets_warn("Failed to set recv timeout: %s", strerror(errno));
    }
    
    /* Receive response - read chunks until we find headers end */
    char header_buffer[8192];
    size_t header_bytes = 0;
    char *headers_end = NULL;
    
    /* Read in chunks until we find \r\n\r\n (end of headers) */
    while (header_bytes < sizeof(header_buffer) - 256) {
        ssize_t n = recv(conn->fd, header_buffer + header_bytes, 
                        sizeof(header_buffer) - header_bytes - 1, 0);
        if (n < 0) {
            buckets_error("Failed to receive response headers: %s", strerror(errno));
            return BUCKETS_ERR_IO;
        }
        if (n == 0) {
            buckets_error("Connection closed before headers received");
            return BUCKETS_ERR_IO;
        }
        header_bytes += n;
        header_buffer[header_bytes] = '\0';
        
        /* Check if we've received complete headers */
        headers_end = strstr(header_buffer, "\r\n\r\n");
        if (headers_end) {
            break;
        }
    }
    
    if (!headers_end) {
        buckets_error("Failed to find end of HTTP headers");
        return BUCKETS_ERR_IO;
    }
    
    /* Parse status code */
    if (status_code) {
        *status_code = 0;
        char *status_line = strstr(header_buffer, "HTTP/1.");
        if (status_line) {
            sscanf(status_line, "HTTP/1.%*d %d", status_code);
        }
    }
    
    /* Parse Content-Length */
    size_t content_length = 0;
    char *cl_header = strstr(header_buffer, "Content-Length:");
    if (cl_header) {
        sscanf(cl_header, "Content-Length: %zu", &content_length);
    }
    
    if (!response) {
        return BUCKETS_OK;
    }
    
    /* Calculate how much body data we already received */
    headers_end += 4;  /* Skip \r\n\r\n */
    size_t body_in_header_buffer = header_bytes - (headers_end - header_buffer);
    
    /* Allocate buffer for complete body */
    char *body_data = NULL;
    size_t total_body_bytes = 0;
    
    if (content_length > 0) {
        /* We know the exact size */
        body_data = buckets_malloc(content_length + 1);
        
        /* Copy body data from header buffer */
        if (body_in_header_buffer > 0) {
            memcpy(body_data, headers_end, body_in_header_buffer);
            total_body_bytes = body_in_header_buffer;
        }
        
        /* Read remaining body */
        while (total_body_bytes < content_length) {
            ssize_t n = recv(conn->fd, body_data + total_body_bytes,
                           content_length - total_body_bytes, 0);
            if (n < 0) {
                buckets_error("Failed to receive response body: %s", strerror(errno));
                buckets_free(body_data);
                return BUCKETS_ERR_IO;
            }
            if (n == 0) {
                break;  /* Connection closed */
            }
            total_body_bytes += n;
        }
        
        body_data[total_body_bytes] = '\0';
    } else {
        /* No Content-Length, use what we have in header buffer */
        if (body_in_header_buffer > 0) {
            body_data = buckets_malloc(body_in_header_buffer + 1);
            memcpy(body_data, headers_end, body_in_header_buffer);
            body_data[body_in_header_buffer] = '\0';
            total_body_bytes = body_in_header_buffer;
        } else {
            body_data = buckets_strdup("");
        }
    }
    
    *response = body_data;
    return BUCKETS_OK;
}
