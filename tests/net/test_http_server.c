/**
 * HTTP Server Tests
 * 
 * Tests for HTTP server functionality using Criterion framework.
 */

#include <criterion/criterion.h>
#include <criterion/redirect.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "buckets.h"
#include "buckets_net.h"

/* ===================================================================
 * Test Fixtures
 * ===================================================================*/

static buckets_http_server_t *server = NULL;

/**
 * Helper to sleep for milliseconds
 */
static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

void setup(void)
{
    buckets_init();
}

void teardown(void)
{
    if (server) {
        buckets_http_server_free(server);
        server = NULL;
    }
    buckets_cleanup();
}

TestSuite(http_server, .init = setup, .fini = teardown);

/* ===================================================================
 * Helper Functions
 * ===================================================================*/

/**
 * Simple HTTP GET request helper
 */
static char* send_http_request(const char *host, int port, const char *path, int *status_code)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return NULL;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &server_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return NULL;
    }
    
    /* Send GET request */
    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host, port);
    
    send(sock, request, strlen(request), 0);
    
    /* Receive response */
    char buffer[4096];
    int total_received = 0;
    int received;
    
    while ((received = recv(sock, buffer + total_received, 
                           sizeof(buffer) - total_received - 1, 0)) > 0) {
        total_received += received;
        if (total_received >= (int)sizeof(buffer) - 1) {
            break;
        }
    }
    
    close(sock);
    
    if (total_received == 0) {
        return NULL;
    }
    
    buffer[total_received] = '\0';
    
    /* Parse status code */
    if (status_code) {
        *status_code = 0;
        char *status_line = strstr(buffer, "HTTP/1.1 ");
        if (status_line) {
            sscanf(status_line, "HTTP/1.1 %d", status_code);
        }
    }
    
    /* Find body (after \r\n\r\n) */
    char *body = strstr(buffer, "\r\n\r\n");
    if (body) {
        body += 4;
        return strdup(body);
    }
    
    return strdup(buffer);
}

/* ===================================================================
 * Test Handlers
 * ===================================================================*/

static void test_handler_hello(buckets_http_request_t *req,
                                buckets_http_response_t *res,
                                void *user_data)
{
    (void)req;
    (void)user_data;
    buckets_http_response_set(res, 200, "Hello, World!", 13);
}

static void test_handler_json(buckets_http_request_t *req,
                               buckets_http_response_t *res,
                               void *user_data)
{
    (void)req;
    (void)user_data;
    buckets_http_response_json(res, 200, "{\"status\":\"ok\"}");
}

static void test_handler_error(buckets_http_request_t *req,
                                buckets_http_response_t *res,
                                void *user_data)
{
    (void)req;
    (void)user_data;
    buckets_http_response_error(res, 500, "Internal server error");
}

/* ===================================================================
 * Tests
 * ===================================================================*/

Test(http_server, create_server)
{
    server = buckets_http_server_create("127.0.0.1", 9999);
    cr_assert_not_null(server, "Server should be created");
}

Test(http_server, create_server_invalid_args)
{
    buckets_http_server_t *s1 = buckets_http_server_create(NULL, 9999);
    cr_assert_null(s1, "Should fail with NULL address");
    
    buckets_http_server_t *s2 = buckets_http_server_create("127.0.0.1", 0);
    cr_assert_null(s2, "Should fail with port 0");
    
    buckets_http_server_t *s3 = buckets_http_server_create("127.0.0.1", -1);
    cr_assert_null(s3, "Should fail with negative port");
    
    buckets_http_server_t *s4 = buckets_http_server_create("127.0.0.1", 99999);
    cr_assert_null(s4, "Should fail with port > 65535");
}

Test(http_server, start_stop_server)
{
    server = buckets_http_server_create("127.0.0.1", 19001);
    cr_assert_not_null(server);
    
    int ret = buckets_http_server_start(server);
    cr_assert_eq(ret, BUCKETS_OK, "Server should start successfully");
    
    /* Give server time to start */
    sleep_ms(100);
    
    ret = buckets_http_server_stop(server);
    cr_assert_eq(ret, BUCKETS_OK, "Server should stop successfully");
}

Test(http_server, get_address)
{
    server = buckets_http_server_create("127.0.0.1", 19002);
    cr_assert_not_null(server);
    
    char addr[256];
    int ret = buckets_http_server_get_address(server, addr, sizeof(addr));
    cr_assert_eq(ret, BUCKETS_OK);
    cr_assert_str_eq(addr, "http://127.0.0.1:19002");
}

Test(http_server, default_handler)
{
    server = buckets_http_server_create("127.0.0.1", 19003);
    cr_assert_not_null(server);
    
    int ret = buckets_http_server_set_default_handler(server, test_handler_hello, NULL);
    cr_assert_eq(ret, BUCKETS_OK);
    
    ret = buckets_http_server_start(server);
    cr_assert_eq(ret, BUCKETS_OK);
    
    sleep_ms(100);
    
    /* Send request */
    int status_code = 0;
    char *response = send_http_request("127.0.0.1", 19003, "/", &status_code);
    
    cr_assert_not_null(response, "Should receive response");
    cr_assert_eq(status_code, 200, "Status should be 200");
    cr_assert_str_eq(response, "Hello, World!");
    
    free(response);
    buckets_http_server_stop(server);
}

Test(http_server, no_handler_returns_404)
{
    server = buckets_http_server_create("127.0.0.1", 19004);
    cr_assert_not_null(server);
    
    int ret = buckets_http_server_start(server);
    cr_assert_eq(ret, BUCKETS_OK);
    
    sleep_ms(100);
    
    int status_code = 0;
    char *response = send_http_request("127.0.0.1", 19004, "/", &status_code);
    
    cr_assert_not_null(response);
    cr_assert_eq(status_code, 404, "Should return 404 when no handler");
    
    free(response);
    buckets_http_server_stop(server);
}

Test(http_server, json_response)
{
    server = buckets_http_server_create("127.0.0.1", 19005);
    cr_assert_not_null(server);
    
    buckets_http_server_set_default_handler(server, test_handler_json, NULL);
    buckets_http_server_start(server);
    
    sleep_ms(100);
    
    int status_code = 0;
    char *response = send_http_request("127.0.0.1", 19005, "/", &status_code);
    
    cr_assert_not_null(response);
    cr_assert_eq(status_code, 200);
    cr_assert_str_eq(response, "{\"status\":\"ok\"}");
    
    free(response);
    buckets_http_server_stop(server);
}

Test(http_server, error_response)
{
    server = buckets_http_server_create("127.0.0.1", 19006);
    cr_assert_not_null(server);
    
    buckets_http_server_set_default_handler(server, test_handler_error, NULL);
    buckets_http_server_start(server);
    
    sleep_ms(100);
    
    int status_code = 0;
    char *response = send_http_request("127.0.0.1", 19006, "/", &status_code);
    
    cr_assert_not_null(response);
    cr_assert_eq(status_code, 500);
    
    free(response);
    buckets_http_server_stop(server);
}

Test(http_server, multiple_requests)
{
    server = buckets_http_server_create("127.0.0.1", 19007);
    cr_assert_not_null(server);
    
    buckets_http_server_set_default_handler(server, test_handler_hello, NULL);
    buckets_http_server_start(server);
    
    sleep_ms(100);
    
    /* Send 5 requests */
    for (int i = 0; i < 5; i++) {
        int status_code = 0;
        char *response = send_http_request("127.0.0.1", 19007, "/", &status_code);
        
        cr_assert_not_null(response);
        cr_assert_eq(status_code, 200);
        cr_assert_str_eq(response, "Hello, World!");
        
        free(response);
    }
    
    buckets_http_server_stop(server);
}

Test(http_server, double_start_fails)
{
    server = buckets_http_server_create("127.0.0.1", 19008);
    cr_assert_not_null(server);
    
    int ret = buckets_http_server_start(server);
    cr_assert_eq(ret, BUCKETS_OK);
    
    sleep_ms(50);
    
    /* Try to start again */
    ret = buckets_http_server_start(server);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail to start twice");
    
    buckets_http_server_stop(server);
}

Test(http_server, enable_tls)
{
    server = buckets_http_server_create("127.0.0.1", 19009);
    cr_assert_not_null(server);
    
    /* Configure TLS */
    buckets_tls_config_t tls_config = {
        .cert_file = "tests/net/certs/cert.pem",
        .key_file = "tests/net/certs/key.pem",
        .ca_file = NULL
    };
    
    int ret = buckets_http_server_enable_tls(server, &tls_config);
    cr_assert_eq(ret, BUCKETS_OK, "Should enable TLS");
    
    /* Verify URL changed to https */
    char addr[256];
    buckets_http_server_get_address(server, addr, sizeof(addr));
    cr_assert(strstr(addr, "https://") != NULL, "URL should use https scheme");
}

Test(http_server, tls_invalid_args)
{
    server = buckets_http_server_create("127.0.0.1", 19010);
    cr_assert_not_null(server);
    
    /* NULL config */
    int ret = buckets_http_server_enable_tls(server, NULL);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail with NULL config");
    
    /* Missing cert file */
    buckets_tls_config_t bad_config = {
        .cert_file = NULL,
        .key_file = "tests/net/certs/key.pem",
        .ca_file = NULL
    };
    
    ret = buckets_http_server_enable_tls(server, &bad_config);
    cr_assert_neq(ret, BUCKETS_OK, "Should fail without cert file");
}

Test(http_server, tls_after_start_fails)
{
    server = buckets_http_server_create("127.0.0.1", 19011);
    cr_assert_not_null(server);
    
    /* Start server first */
    int ret = buckets_http_server_start(server);
    cr_assert_eq(ret, BUCKETS_OK);
    
    sleep_ms(50);
    
    /* Try to enable TLS after start */
    buckets_tls_config_t tls_config = {
        .cert_file = "tests/net/certs/cert.pem",
        .key_file = "tests/net/certs/key.pem",
        .ca_file = NULL
    };
    
    ret = buckets_http_server_enable_tls(server, &tls_config);
    cr_assert_neq(ret, BUCKETS_OK, "Should not allow TLS after start");
    
    buckets_http_server_stop(server);
}
