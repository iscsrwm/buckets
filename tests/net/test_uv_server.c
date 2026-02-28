/**
 * Test UV HTTP Server
 * 
 * Basic tests for the libuv-based HTTP server.
 */

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE  /* For usleep */
#endif
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
#include "../src/net/uv_server_internal.h"

/* Test port - use a high port to avoid conflicts */
#define TEST_PORT 19876

/* Simple test handler */
static void test_handler(uv_http_conn_t *conn, void *user_data)
{
    (void)user_data;
    
    const char *method = llhttp_method_name(llhttp_get_method(&conn->parser));
    
    /* Echo back the request info */
    char response_body[4096];
    int body_len = snprintf(response_body, sizeof(response_body),
        "Method: %s\n"
        "URL: %s\n"
        "Body Length: %zu\n",
        method,
        conn->url ? conn->url : "(null)",
        conn->body_len);
    
    const char *headers[] = {
        "Content-Type", "text/plain",
        NULL
    };
    
    uv_http_response_start(conn, 200, headers, 2, body_len);
    uv_http_response_write(conn, response_body, body_len);
}

/* Helper: send HTTP request and get response */
static int send_request(const char *method, const char *path, 
                        const char *body, size_t body_len,
                        char *response, size_t response_size)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    
    /* Send request */
    char request[4096];
    int req_len;
    
    if (body && body_len > 0) {
        req_len = snprintf(request, sizeof(request),
            "%s %s HTTP/1.1\r\n"
            "Host: localhost:%d\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path, TEST_PORT, body_len);
    } else {
        req_len = snprintf(request, sizeof(request),
            "%s %s HTTP/1.1\r\n"
            "Host: localhost:%d\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path, TEST_PORT);
    }
    
    if (send(sock, request, req_len, 0) != req_len) {
        perror("send headers");
        close(sock);
        return -1;
    }
    
    if (body && body_len > 0) {
        if (send(sock, body, body_len, 0) != (ssize_t)body_len) {
            perror("send body");
            close(sock);
            return -1;
        }
    }
    
    /* Receive response */
    size_t total = 0;
    ssize_t n;
    while ((n = recv(sock, response + total, response_size - total - 1, 0)) > 0) {
        total += n;
    }
    response[total] = '\0';
    
    close(sock);
    return (int)total;
}

/* Test: basic GET request */
static int test_basic_get(void)
{
    char response[4096];
    
    int len = send_request("GET", "/test/path", NULL, 0, response, sizeof(response));
    if (len < 0) {
        printf("FAIL: Failed to send request\n");
        return 1;
    }
    
    /* Check response */
    if (strstr(response, "HTTP/1.1 200") == NULL) {
        printf("FAIL: Expected 200 OK, got:\n%s\n", response);
        return 1;
    }
    
    if (strstr(response, "Method: GET") == NULL) {
        printf("FAIL: Expected Method: GET in response\n");
        return 1;
    }
    
    if (strstr(response, "URL: /test/path") == NULL) {
        printf("FAIL: Expected URL: /test/path in response\n");
        return 1;
    }
    
    printf("PASS: test_basic_get\n");
    return 0;
}

/* Test: POST with body */
static int test_post_with_body(void)
{
    char response[4096];
    const char *body = "Hello, World!";
    
    int len = send_request("POST", "/api/data", body, strlen(body), response, sizeof(response));
    if (len < 0) {
        printf("FAIL: Failed to send request\n");
        return 1;
    }
    
    if (strstr(response, "HTTP/1.1 200") == NULL) {
        printf("FAIL: Expected 200 OK\n");
        return 1;
    }
    
    if (strstr(response, "Method: POST") == NULL) {
        printf("FAIL: Expected Method: POST\n");
        return 1;
    }
    
    if (strstr(response, "Body Length: 13") == NULL) {
        printf("FAIL: Expected Body Length: 13\n");
        return 1;
    }
    
    printf("PASS: test_post_with_body\n");
    return 0;
}

/* Test: keep-alive connection */
static int test_keep_alive(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    
    /* Send first request with keep-alive */
    const char *req1 = 
        "GET /first HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    
    if (send(sock, req1, strlen(req1), 0) < 0) {
        perror("send");
        close(sock);
        return 1;
    }
    
    /* Read first response */
    char response[4096];
    usleep(100000);  /* Wait for response */
    int n = recv(sock, response, sizeof(response) - 1, MSG_DONTWAIT);
    if (n <= 0) {
        printf("FAIL: No response to first request\n");
        close(sock);
        return 1;
    }
    response[n] = '\0';
    
    if (strstr(response, "HTTP/1.1 200") == NULL) {
        printf("FAIL: First request failed\n");
        close(sock);
        return 1;
    }
    
    if (strstr(response, "Connection: keep-alive") == NULL) {
        printf("FAIL: First response missing Connection: keep-alive\n");
        close(sock);
        return 1;
    }
    
    /* Send second request on same connection */
    const char *req2 = 
        "GET /second HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    
    if (send(sock, req2, strlen(req2), 0) < 0) {
        printf("FAIL: Connection closed after first request (keep-alive not working)\n");
        close(sock);
        return 1;
    }
    
    /* Read second response */
    usleep(100000);
    n = recv(sock, response, sizeof(response) - 1, MSG_DONTWAIT);
    if (n <= 0) {
        printf("FAIL: No response to second request\n");
        close(sock);
        return 1;
    }
    response[n] = '\0';
    
    if (strstr(response, "HTTP/1.1 200") == NULL) {
        printf("FAIL: Second request failed\n");
        close(sock);
        return 1;
    }
    
    close(sock);
    printf("PASS: test_keep_alive\n");
    return 0;
}

/* ===================================================================
 * Streaming Handler Tests
 * ===================================================================*/

/* State for tracking streaming callback invocations */
static struct {
    int on_start_called;
    int on_body_called;
    int on_complete_called;
    int on_error_called;
    size_t total_body_bytes;
    char received_data[65536];
    size_t received_len;
} stream_test_state;

static void reset_stream_test_state(void)
{
    memset(&stream_test_state, 0, sizeof(stream_test_state));
}

/* Test streaming handler callbacks */
static int stream_on_start(uv_stream_request_t *req, void *user_data)
{
    (void)user_data;
    stream_test_state.on_start_called++;
    
    /* Verify we can access request info */
    if (req->method != HTTP_PUT) {
        printf("  ERROR: Expected PUT method\n");
        return -1;
    }
    
    /* Accept the streaming request */
    return 0;
}

static int stream_on_body(uv_stream_request_t *req, const void *data, size_t len, void *user_data)
{
    (void)req;
    (void)user_data;
    stream_test_state.on_body_called++;
    stream_test_state.total_body_bytes += len;
    
    /* Copy received data (up to buffer limit) */
    if (stream_test_state.received_len + len < sizeof(stream_test_state.received_data)) {
        memcpy(stream_test_state.received_data + stream_test_state.received_len, data, len);
        stream_test_state.received_len += len;
    }
    
    return 0;
}

static int stream_on_complete(uv_stream_request_t *req, void *user_data)
{
    (void)user_data;
    stream_test_state.on_complete_called++;
    
    /* Send success response */
    const char *headers[] = {
        "Content-Type", "text/plain",
        NULL
    };
    
    char body[256];
    int body_len = snprintf(body, sizeof(body),
        "Received %zu bytes in %d chunks",
        stream_test_state.total_body_bytes,
        stream_test_state.on_body_called);
    
    uv_http_response_start(req->conn, 200, headers, 2, body_len);
    uv_http_response_write(req->conn, body, body_len);
    
    return 0;
}

static void stream_on_error(uv_stream_request_t *req, int error, void *user_data)
{
    (void)req;
    (void)error;
    (void)user_data;
    stream_test_state.on_error_called++;
}

/* Test: streaming PUT request */
static int test_streaming_put(uv_http_server_t *server)
{
    reset_stream_test_state();
    
    /* Register streaming handler for PUT /stream/ */
    uv_stream_handler_t handler;
    handler.on_request_start = stream_on_start;
    handler.on_body_chunk = stream_on_body;
    handler.on_request_complete = stream_on_complete;
    handler.on_request_error = stream_on_error;
    handler.user_data = NULL;
    
    int ret = uv_http_server_add_streaming_route(server, "PUT", "/stream/", &handler);
    if (ret != 0) {
        printf("FAIL: Failed to add streaming route\n");
        return 1;
    }
    
    /* Connect and send PUT request */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("FAIL: socket() failed\n");
        return 1;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("FAIL: connect() failed\n");
        close(sock);
        return 1;
    }
    
    /* Send PUT request with 10KB body */
    const char *headers =
        "PUT /stream/test-object HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 10240\r\n"
        "Connection: close\r\n"
        "\r\n";
    
    if (send(sock, headers, strlen(headers), 0) < 0) {
        printf("FAIL: send headers failed\n");
        close(sock);
        return 1;
    }
    
    /* Send body in multiple chunks to test streaming */
    char chunk[1024];
    memset(chunk, 'A', sizeof(chunk));
    
    for (int i = 0; i < 10; i++) {
        if (send(sock, chunk, sizeof(chunk), 0) != sizeof(chunk)) {
            printf("FAIL: send chunk %d failed\n", i);
            close(sock);
            return 1;
        }
        usleep(10000);  /* 10ms delay between chunks */
    }
    
    /* Read response */
    char response[4096];
    usleep(100000);  /* Wait for processing */
    
    size_t total = 0;
    ssize_t n;
    while ((n = recv(sock, response + total, sizeof(response) - total - 1, 0)) > 0) {
        total += n;
    }
    response[total] = '\0';
    
    close(sock);
    
    /* Verify callbacks were called */
    if (stream_test_state.on_start_called != 1) {
        printf("FAIL: on_start called %d times (expected 1)\n",
               stream_test_state.on_start_called);
        return 1;
    }
    
    if (stream_test_state.on_body_called < 1) {
        printf("FAIL: on_body never called\n");
        return 1;
    }
    
    if (stream_test_state.on_complete_called != 1) {
        printf("FAIL: on_complete called %d times (expected 1)\n",
               stream_test_state.on_complete_called);
        return 1;
    }
    
    if (stream_test_state.total_body_bytes != 10240) {
        printf("FAIL: received %zu bytes (expected 10240)\n",
               stream_test_state.total_body_bytes);
        return 1;
    }
    
    /* Verify response */
    if (strstr(response, "HTTP/1.1 200") == NULL) {
        printf("FAIL: Expected 200 OK, got:\n%s\n", response);
        return 1;
    }
    
    if (strstr(response, "Received 10240 bytes") == NULL) {
        printf("FAIL: Response doesn't confirm receipt\n%s\n", response);
        return 1;
    }
    
    printf("PASS: test_streaming_put (received in %d chunks)\n",
           stream_test_state.on_body_called);
    return 0;
}

/* Test: streaming PUT with large body (1MB) */
static int test_streaming_large_put(uv_http_server_t *server)
{
    (void)server;  /* Already has streaming route registered */
    reset_stream_test_state();
    
    const size_t body_size = 1024 * 1024;  /* 1MB */
    
    /* Connect and send PUT request */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("FAIL: socket() failed\n");
        return 1;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("FAIL: connect() failed\n");
        close(sock);
        return 1;
    }
    
    /* Send PUT request with 1MB body */
    char headers_buf[256];
    snprintf(headers_buf, sizeof(headers_buf),
        "PUT /stream/large-object HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n", body_size);
    
    if (send(sock, headers_buf, strlen(headers_buf), 0) < 0) {
        printf("FAIL: send headers failed\n");
        close(sock);
        return 1;
    }
    
    /* Send body in 64KB chunks */
    char *chunk = malloc(65536);
    if (!chunk) {
        printf("FAIL: malloc failed\n");
        close(sock);
        return 1;
    }
    memset(chunk, 'B', 65536);
    
    size_t sent = 0;
    while (sent < body_size) {
        size_t to_send = (body_size - sent < 65536) ? (body_size - sent) : 65536;
        ssize_t n = send(sock, chunk, to_send, 0);
        if (n < 0) {
            printf("FAIL: send chunk failed at %zu bytes\n", sent);
            free(chunk);
            close(sock);
            return 1;
        }
        sent += n;
    }
    free(chunk);
    
    /* Read response */
    char response[4096];
    usleep(200000);  /* Wait for processing */
    
    size_t total = 0;
    ssize_t n;
    while ((n = recv(sock, response + total, sizeof(response) - total - 1, 0)) > 0) {
        total += n;
    }
    response[total] = '\0';
    
    close(sock);
    
    /* Verify */
    if (stream_test_state.total_body_bytes != body_size) {
        printf("FAIL: received %zu bytes (expected %zu)\n",
               stream_test_state.total_body_bytes, body_size);
        return 1;
    }
    
    if (strstr(response, "HTTP/1.1 200") == NULL) {
        printf("FAIL: Expected 200 OK\n");
        return 1;
    }
    
    printf("PASS: test_streaming_large_put (1MB in %d chunks)\n",
           stream_test_state.on_body_called);
    return 0;
}

/* Test: multiple keep-alive requests */
static int test_multiple_keep_alive(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    
    char response[4096];
    int n;
    
    /* Send 5 requests on the same connection */
    for (int i = 0; i < 5; i++) {
        char request[256];
        snprintf(request, sizeof(request),
            "GET /request%d HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n", i);
        
        if (send(sock, request, strlen(request), 0) < 0) {
            printf("FAIL: Failed to send request %d\n", i);
            close(sock);
            return 1;
        }
        
        usleep(50000);
        n = recv(sock, response, sizeof(response) - 1, MSG_DONTWAIT);
        if (n <= 0) {
            printf("FAIL: No response to request %d\n", i);
            close(sock);
            return 1;
        }
        response[n] = '\0';
        
        if (strstr(response, "HTTP/1.1 200") == NULL) {
            printf("FAIL: Request %d failed\n", i);
            close(sock);
            return 1;
        }
    }
    
    close(sock);
    printf("PASS: test_multiple_keep_alive\n");
    return 0;
}

int main(void)
{
    printf("=== UV HTTP Server Tests ===\n\n");
    
    /* Create server */
    uv_http_server_t *server = uv_http_server_create("127.0.0.1", TEST_PORT);
    if (!server) {
        printf("FAIL: Failed to create server\n");
        return 1;
    }
    
    /* Set handler */
    uv_http_server_set_handler(server, test_handler, NULL);
    
    /* Start server */
    if (uv_http_server_start(server) != BUCKETS_OK) {
        printf("FAIL: Failed to start server\n");
        uv_http_server_free(server);
        return 1;
    }
    
    printf("Server started on port %d\n\n", TEST_PORT);
    
    /* Wait for server to be ready */
    usleep(100000);
    
    /* Run tests */
    int failures = 0;
    
    failures += test_basic_get();
    failures += test_post_with_body();
    failures += test_keep_alive();
    failures += test_multiple_keep_alive();
    failures += test_streaming_put(server);
    failures += test_streaming_large_put(server);
    
    /* Stop server */
    printf("\nStopping server...\n");
    uv_http_server_stop(server);
    uv_http_server_free(server);
    
    printf("\n=== Results: %d failures ===\n", failures);
    
    return failures > 0 ? 1 : 0;
}
