/**
 * Connection Pool Tests
 * 
 * Tests for TCP connection pooling using Criterion framework.
 */

#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "buckets.h"
#include "buckets_net.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

static buckets_conn_pool_t *pool = NULL;
static int test_server_sock = -1;
static int test_server_port = 0;
static pthread_t server_thread;
static bool server_running = false;

/**
 * Simple TCP echo server for testing
 */
static void* test_server_thread_func(void *arg)
{
    (void)arg;
    
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(test_server_sock, 
                                 (struct sockaddr*)&client_addr,
                                 &client_len);
        if (client_sock < 0) {
            continue;
        }
        
        /* Echo back whatever we receive */
        char buffer[1024];
        ssize_t received = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
        if (received > 0) {
            send(client_sock, buffer, received, 0);
        }
        
        close(client_sock);
    }
    
    return NULL;
}

/**
 * Start test server on random port
 */
static int start_test_server(void)
{
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);
    
    /* Create socket */
    test_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (test_server_sock < 0) {
        return -1;
    }
    
    /* Allow reuse */
    int opt = 1;
    setsockopt(test_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /* Bind to any available port */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = 0;  /* Let OS choose */
    
    if (bind(test_server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(test_server_sock);
        return -1;
    }
    
    /* Get assigned port */
    if (getsockname(test_server_sock, (struct sockaddr*)&server_addr, &addr_len) < 0) {
        close(test_server_sock);
        return -1;
    }
    test_server_port = ntohs(server_addr.sin_port);
    
    /* Listen */
    if (listen(test_server_sock, 10) < 0) {
        close(test_server_sock);
        return -1;
    }
    
    /* Start server thread */
    server_running = true;
    if (pthread_create(&server_thread, NULL, test_server_thread_func, NULL) != 0) {
        close(test_server_sock);
        return -1;
    }
    
    return 0;
}

/**
 * Stop test server
 */
static void stop_test_server(void)
{
    if (server_running) {
        server_running = false;
        
        /* Connect to trigger accept() to unblock */
        int dummy = socket(AF_INET, SOCK_STREAM, 0);
        if (dummy >= 0) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            addr.sin_port = htons(test_server_port);
            connect(dummy, (struct sockaddr*)&addr, sizeof(addr));
            close(dummy);
        }
        
        pthread_join(server_thread, NULL);
        close(test_server_sock);
    }
}

void setup(void)
{
    buckets_init();
    start_test_server();
}

void teardown(void)
{
    if (pool) {
        buckets_conn_pool_free(pool);
        pool = NULL;
    }
    stop_test_server();
    buckets_cleanup();
}

TestSuite(conn_pool, .init = setup, .fini = teardown);

/* ===================================================================
 * Tests
 * ===================================================================*/

Test(conn_pool, create_pool)
{
    pool = buckets_conn_pool_create(10);
    cr_assert_not_null(pool, "Pool should be created");
    
    int total, active, idle;
    int ret = buckets_conn_pool_stats(pool, &total, &active, &idle);
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(total, 0, "New pool should have 0 connections");
    cr_assert_eq(active, 0);
    cr_assert_eq(idle, 0);
}

Test(conn_pool, create_unlimited_pool)
{
    pool = buckets_conn_pool_create(0);  /* 0 = unlimited */
    cr_assert_not_null(pool, "Unlimited pool should be created");
}

Test(conn_pool, get_connection)
{
    pool = buckets_conn_pool_create(10);
    cr_assert_not_null(pool);
    
    buckets_connection_t *conn = NULL;
    int ret = buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn);
    
    cr_assert_eq(ret, BUCKETS_OK, "Should get connection");
    cr_assert_not_null(conn, "Connection should not be NULL");
    
    int total, active, idle;
    buckets_conn_pool_stats(pool, &total, &active, &idle);
    cr_assert_eq(total, 1, "Should have 1 total connection");
    cr_assert_eq(active, 1, "Should have 1 active connection");
    cr_assert_eq(idle, 0, "Should have 0 idle connections");
}

Test(conn_pool, release_connection)
{
    pool = buckets_conn_pool_create(10);
    cr_assert_not_null(pool);
    
    buckets_connection_t *conn = NULL;
    buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn);
    cr_assert_not_null(conn);
    
    int ret = buckets_conn_pool_release(pool, conn);
    cr_assert_eq(ret, BUCKETS_OK, "Should release connection");
    
    int total, active, idle;
    buckets_conn_pool_stats(pool, &total, &active, &idle);
    cr_assert_eq(total, 1);
    cr_assert_eq(active, 0, "Should have 0 active after release");
    cr_assert_eq(idle, 1, "Should have 1 idle after release");
}

Test(conn_pool, reuse_connection)
{
    pool = buckets_conn_pool_create(10);
    cr_assert_not_null(pool);
    
    /* Get connection */
    buckets_connection_t *conn1 = NULL;
    buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn1);
    cr_assert_not_null(conn1);
    
    /* Release it */
    buckets_conn_pool_release(pool, conn1);
    
    /* Get connection again - should reuse */
    buckets_connection_t *conn2 = NULL;
    buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn2);
    cr_assert_not_null(conn2);
    
    /* Should be the same connection */
    cr_assert_eq(conn1, conn2, "Should reuse connection");
    
    int total, active, idle;
    buckets_conn_pool_stats(pool, &total, &active, &idle);
    cr_assert_eq(total, 1, "Should still have 1 total connection");
}

Test(conn_pool, multiple_connections)
{
    pool = buckets_conn_pool_create(10);
    cr_assert_not_null(pool);
    
    buckets_connection_t *conn1 = NULL;
    buckets_connection_t *conn2 = NULL;
    buckets_connection_t *conn3 = NULL;
    
    buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn1);
    buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn2);
    buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn3);
    
    cr_assert_not_null(conn1);
    cr_assert_not_null(conn2);
    cr_assert_not_null(conn3);
    
    /* Should be different connections */
    cr_assert_neq(conn1, conn2);
    cr_assert_neq(conn2, conn3);
    cr_assert_neq(conn1, conn3);
    
    int total, active, idle;
    buckets_conn_pool_stats(pool, &total, &active, &idle);
    cr_assert_eq(total, 3);
    cr_assert_eq(active, 3);
    cr_assert_eq(idle, 0);
}

Test(conn_pool, pool_limit)
{
    pool = buckets_conn_pool_create(2);  /* Limit to 2 connections */
    cr_assert_not_null(pool);
    
    buckets_connection_t *conn1 = NULL;
    buckets_connection_t *conn2 = NULL;
    buckets_connection_t *conn3 = NULL;
    
    int ret;
    ret = buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn1);
    cr_assert_eq(ret, BUCKETS_OK);
    
    ret = buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn2);
    cr_assert_eq(ret, BUCKETS_OK);
    
    /* Third should fail - pool limit reached */
    ret = buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn3);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail when pool limit reached");
}

Test(conn_pool, close_connection)
{
    pool = buckets_conn_pool_create(10);
    cr_assert_not_null(pool);
    
    buckets_connection_t *conn = NULL;
    buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn);
    cr_assert_not_null(conn);
    
    int ret = buckets_conn_pool_close(pool, conn);
    cr_assert_eq(ret, BUCKETS_OK, "Should close connection");
    
    int total, active, idle;
    buckets_conn_pool_stats(pool, &total, &active, &idle);
    cr_assert_eq(total, 0, "Should have 0 connections after close");
}

Test(conn_pool, invalid_args)
{
    pool = buckets_conn_pool_create(10);
    cr_assert_not_null(pool);
    
    buckets_connection_t *conn = NULL;
    
    /* NULL pool */
    int ret = buckets_conn_pool_get(NULL, "127.0.0.1", test_server_port, &conn);
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* NULL host */
    ret = buckets_conn_pool_get(pool, NULL, test_server_port, &conn);
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* Invalid port */
    ret = buckets_conn_pool_get(pool, "127.0.0.1", 0, &conn);
    cr_assert_neq(ret, BUCKETS_OK);
    
    /* NULL conn */
    ret = buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, NULL);
    cr_assert_neq(ret, BUCKETS_OK);
}

Test(conn_pool, stats)
{
    pool = buckets_conn_pool_create(10);
    cr_assert_not_null(pool);
    
    int total, active, idle;
    int ret = buckets_conn_pool_stats(pool, &total, &active, &idle);
    
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_eq(total, 0);
    cr_assert_eq(active, 0);
    cr_assert_eq(idle, 0);
    
    /* Add some connections */
    buckets_connection_t *conn1 = NULL;
    buckets_connection_t *conn2 = NULL;
    
    buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn1);
    buckets_conn_pool_get(pool, "127.0.0.1", test_server_port, &conn2);
    
    buckets_conn_pool_stats(pool, &total, &active, &idle);
    cr_assert_eq(total, 2);
    cr_assert_eq(active, 2);
    cr_assert_eq(idle, 0);
    
    /* Release one */
    buckets_conn_pool_release(pool, conn1);
    
    buckets_conn_pool_stats(pool, &total, &active, &idle);
    cr_assert_eq(total, 2);
    cr_assert_eq(active, 1);
    cr_assert_eq(idle, 1);
}
